#ifndef MC2_INPUT_CAPTURE_POLICY_H
#define MC2_INPUT_CAPTURE_POLICY_H

// I3: the pure capture/focus/modal decisions of the input layer.
//
// Everything here is a function of booleans and the previous key/button
// state -- no SDL, no Win32, no globals. The acquisition adapters
// (SDL_GetMouseState / GetAsyncKeyState / SDL_GetKeyboardState) stay in
// gos_input.cpp; this header only decides what the acquired state means.
// Keeping the decisions pure lets the event-sequence matrix in
// tests/unit/test_input_capture_policy.cpp lock them without a device.

namespace input {
namespace capture {

// The three per-frame flags the game and the ImGui layer publish.
struct PolicyState {
    bool imguiWantsMouse  = false;  // ImGui::GetIO().WantCaptureMouse
    bool imguiHoversMouse = false;  // ImGui::IsWindowHovered(AnyWindow)
    bool gameModalActive  = false;  // legacy modal state; cannot override capture
};

// ImGui owns captured input even over the legacy pause menu.
inline bool mouseButtonsReachGame(const PolicyState& s) {
    return !s.imguiWantsMouse;
}

// Wheel follows ImGui hover or capture, including during legacy modals.
inline bool wheelReachesGame(const PolicyState& s) {
    return !s.imguiHoversMouse && !s.imguiWantsMouse;
}

// Press/release edge kernel. Values match input::KeyState in gos_input.h
// (KS_FREE/KS_PRESSED/KS_HELD/KS_RELEASED); gos_input.cpp static_asserts it.
enum class Edge : int {
    Free     = 0,
    Pressed  = 1,
    Held     = 2,
    Released = 3
};

inline Edge nextEdge(Edge prev, bool down) {
    if (down)
        return (prev == Edge::Free || prev == Edge::Released) ? Edge::Pressed : Edge::Held;
    return (prev == Edge::Held || prev == Edge::Pressed) ? Edge::Released : Edge::Free;
}

// Swallow the entire captured key gesture, including its release. Closing a
// panel while a key is held must not manufacture a game shortcut.
inline Edge keyboardEdge(Edge previous, bool down, bool captured, bool& blocked) {
    if (captured) blocked = down;
    if (blocked || captured) {
        blocked = down;
        return Edge::Free;
    }
    return nextEdge(previous, down);
}

// A gesture belongs to whoever received its press, even after leaving that
// surface. Focus regain quarantines the activation buttons until released.
struct PointerCapture {
    bool focused = true;
    bool quarantine = false;
    unsigned held = 0;
    unsigned blocked = 0;

    void claim() { blocked |= held; }

    void setFocus(bool value) {
        if (focused != value) quarantine = true;
        focused = value;
    }
    unsigned sample(unsigned buttons, bool allowPress) {
        if (!focused || quarantine) blocked |= buttons;
        if (!allowPress) blocked |= buttons & ~held;
        blocked &= buttons;
        held = buttons;
        quarantine = !focused;
        return focused ? buttons & ~blocked : 0;
    }
    Edge edge(Edge previous, unsigned mask, unsigned accepted) const {
        if (!focused || (blocked & mask)) return Edge::Free;
        return nextEdge(previous, (accepted & mask) != 0);
    }
};

// Logistics widgets capture from the press location, not the moving pointer.
inline bool startsWidgetDrag(bool dragging, bool anchorInside) {
    return dragging && anchorInside;
}

} // namespace capture
} // namespace input

#endif // MC2_INPUT_CAPTURE_POLICY_H
