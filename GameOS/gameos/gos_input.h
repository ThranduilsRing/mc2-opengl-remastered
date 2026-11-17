#ifndef GOS_INPUT_H
#define GOS_INPUT_H

// Self-containment: signatures below use SDL_Event*/SDL_Keysym. mc2/viewer
// only compiled by include-order luck; EditRel includes this header first.
// (Additive include hardening, from PR #36.)
#include <SDL2/SDL.h>
#include <cstdint>

namespace input {

enum class KeyState : int {
    KS_FREE = 0, // in not pressed state
    KS_PRESSED, // just pressed
    KS_HELD,    // in pressed state 
    KS_RELEASED // just released
};
using enum KeyState;

struct MouseInfo {
    MouseInfo();

    static const int NUM_BUTTONS = 5;
    float x_;
    float y_;
    float rel_x_;
    float rel_y_;
    float wheel_vert_;
    float wheel_hor_;
    KeyState button_state_[NUM_BUTTONS];
};

struct KeyboardInfo {
    KeyboardInfo();
    bool key_pressed_;
    bool key_released_;
    SDL_Keysym pressed_keysym_;
    SDL_Keysym released_keysym_;
    uint32_t first_pressed_;
    uint8_t last_state_[512];
};

const MouseInfo* getMouseInfo();
const KeyboardInfo* getKeyboardInfo();

// Test-only: inject a synthetic raw SDL mouse point (window-points space).
void setMouseInfoRaw(float x, float y);

void handleMouseMotion(const SDL_Event* event);
void handleMouseButton(const SDL_Event* event);
void handleMouseWheel(const SDL_Event* event);
void beginUpdateMouseState();
void updateMouseState();

void handleKeyEvent(const SDL_Event* event);
void updateKeyboardState();

void resetKeypress();
void setInputFocus(bool focused);

// MC2_INPUT_SCRIPT (scripted player, see gos_input.cpp): true + normalised 0..1 position once the script has moved the pointer.
bool scriptMouseNorm(float* nx, float* ny, bool* buttons = nullptr);

// Set by the ImGui layer each frame after NewFrame(); gates game mouse input.
// The capture/focus/modal decisions these flags feed live in
// input_capture_policy.h (pure, SDL-free); this header only publishes them.
void setImguiWantsMouse(bool v);
void setImguiWantsKeyboard(bool v);

// HUD-IMGUI wheel guard (IN-MISSION-HUD-IMGUI-SCOPE-1 slice E, fixes slice D's
// deferred wheel-zoom conflict for ALL ImGui HUD panels): set per-frame from
// ImGui::IsWindowHovered(AnyWindow). Purely spatial — deliberately NOT
// WantCaptureMouse, which is tainted by mouse_any_down (true for the whole
// duration of any click, even over the 3D viewport) and would eat wheel-zoom
// during every drag. When true, SDL wheel events are not forwarded to the
// game (camera zoom); ImGui still receives them through its own backend.
// One frame of hover staleness is acceptable (hover is spatial/slow).
void setImguiHoversMouse(bool v);

// Legacy modal state remains published; it cannot override ImGui input ownership.
void setGameModalActive(bool v);

}

#endif //GOS_INPUT_H
