// I3: the pure input capture policy (GameOS/gameos/input_capture_policy.h).
//
// The acquisition adapters (SDL_GetMouseState, GetAsyncKeyState,
// SDL_GetKeyboardState) stay in gos_input.cpp; this tier locks the decisions
// they feed -- the ImGui capture gates, the game-modal exception, and the
// press/release edge kernel -- against a recorded event-sequence matrix.

#include "doctest.h"

#include "input_capture_policy.h"
#include "../../GuiRuntime/GuiWindowPolicy.h"

// Trigger paths: GuiRuntime.cpp, GuiWindowPolicy.h, gos_input.cpp, input_capture_policy.h.
TEST_CASE("input_capture_policy: keyboard capture swallows holds and resumes on a fresh press") {
    using input::capture::Edge;
    bool blocked = false;
    using input::capture::keyboardEdge;
    CHECK(keyboardEdge(Edge::Free, true, true, blocked) == Edge::Free);
    CHECK(keyboardEdge(Edge::Free, true, false, blocked) == Edge::Free);
    CHECK(keyboardEdge(Edge::Free, false, false, blocked) == Edge::Free);
    CHECK(keyboardEdge(Edge::Free, true, false, blocked) == Edge::Pressed);
    CHECK(keyboardEdge(Edge::Held, true, true, blocked) == Edge::Free);
    CHECK(keyboardEdge(Edge::Free, false, true, blocked) == Edge::Free);
    CHECK(keyboardEdge(Edge::Free, true, false, blocked) == Edge::Pressed);
    CHECK(keyboardEdge(Edge::Pressed, false, false, blocked) == Edge::Released);
}

TEST_CASE("input_capture_policy: saved windows fit the work area after resolution changes") {
    using gui_window_policy::clampPosition;
    CHECK(clampPosition(3600, 500, 0, 1920) == 1420);
    CHECK(clampPosition(-300, 500, 0, 1920) == -300);
    CHECK(clampPosition(200, 500, 0, 1920) == 200);
    CHECK(clampPosition(2100, 500, 100, 1920) == 1520);
    CHECK(clampPosition(-100, 2500, 100, 1920) == -100);
    CHECK(clampPosition(NAN, 500, 100, 1920) == 100);
    CHECK(clampPosition(200, 500, 100, 0) == 200);
    CHECK(gui_window_policy::scaleForHeight(1080) == 1);
    CHECK(gui_window_policy::scaleForHeight(2160) == 2);
    CHECK(gui_window_policy::scaleForHeight(0) == 1);
}

using input::capture::Edge;
using input::capture::PolicyState;
using input::capture::mouseButtonsReachGame;
using input::capture::nextEdge;
using input::capture::wheelReachesGame;
using input::capture::PointerCapture;
using input::capture::startsWidgetDrag;

// Trigger paths: gos_input.cpp, gameosmain.cpp, userinput.cpp,
// componentlistbox.cpp, mechlabscreen.cpp and input_capture_policy.h.
TEST_CASE("input_capture_policy: logistics down move outside source up delivers drag") {
    PointerCapture pointer;
    Edge edge = pointer.edge(Edge::Free, 1, pointer.sample(1, true));
    REQUIRE(edge == Edge::Pressed);
    // The next sample has moved past the inventory item (or mech diagram).
    // ImGui capture changing during the gesture must not steal the hold.
    edge = pointer.edge(edge, 1, pointer.sample(1, false));
    pointer.claim(); // ImGui's current-frame capture cannot steal an older press.
    CHECK(pointer.edge(edge, 1, 1) == Edge::Held);
    REQUIRE(edge == Edge::Held);
    CHECK(startsWidgetDrag(edge == Edge::Held, true));
    CHECK_FALSE(startsWidgetDrag(edge == Edge::Held, false));
    edge = pointer.edge(edge, 1, pointer.sample(0, false));
    CHECK(edge == Edge::Released); // the logistics endDrag delivery
    CHECK_FALSE(startsWidgetDrag(false, true));
}

TEST_CASE("input_capture_policy: captured press cannot turn into a game drag") {
    PointerCapture pointer;
    CHECK(pointer.sample(1, false) == 0);
    CHECK(pointer.edge(Edge::Free, 1, 0) == Edge::Free);
    CHECK(pointer.sample(0, false) == 0);
    // Previous-frame capture can be false at the press; NewFrame claims it.
    CHECK(pointer.sample(1, true) == 1);
    pointer.claim();
    CHECK(pointer.edge(Edge::Free, 1, 1) == Edge::Free);
    CHECK(pointer.sample(1, true) == 0); // left the ImGui surface while held
    CHECK(pointer.sample(0, true) == 0);
    CHECK(pointer.sample(1, true) == 1); // a fresh game press
    pointer.claim(); // ImGui opens while a game button is held.
    CHECK(pointer.edge(Edge::Held, 1, pointer.sample(1, true)) == Edge::Free);
    CHECK(pointer.edge(Edge::Free, 1, pointer.sample(0, true)) == Edge::Free);
    CHECK(pointer.edge(Edge::Free, 1, pointer.sample(1, true)) == Edge::Pressed);
}

TEST_CASE("input_capture_policy: focus cycle preserves selection and swallows activation") {
    PointerCapture pointer;
    Edge edge = Edge::Free;
    // No Pressed or Released means neither single-select nor mouse-up order.
    // Also cover alt-tab interrupting a held game button.
    for (bool wasHeld : {false, true}) {
        edge = wasHeld ? Edge::Held : Edge::Free;
        pointer.setFocus(false);
        edge = pointer.edge(edge, 1, pointer.sample(1, true));
        CHECK(edge == Edge::Free);
        pointer.setFocus(true);
        edge = pointer.edge(edge, 1, pointer.sample(1, true));
        CHECK(edge == Edge::Free);
        edge = pointer.edge(edge, 1, pointer.sample(0, true));
        CHECK(edge == Edge::Free);
        edge = pointer.edge(edge, 1, pointer.sample(1, true));
        CHECK(edge == Edge::Pressed); // next deliberate click still works
    }
    // Both focus events can arrive in one SDL pump, with no intervening frame.
    pointer.setFocus(false);
    pointer.setFocus(true);
    CHECK(pointer.sample(1, true) == 0);
}

TEST_CASE("input_capture_policy: mouse buttons reach the game unless ImGui captured them") {
    // imguiWantsMouse x gameModalActive, recorded matrix.
    struct Row { bool wants; bool modal; bool expected; };
    const Row rows[] = {
        { false, false, true  },  // nothing captured: game gets the click
        { false, true,  true  },  // modal, no ImGui capture: game
        { true,  false, false },  // ImGui captured: gated out
        { true,  true,  true  },  // legacy pause buttons must remain clickable
    };
    for (const Row& r : rows) {
        CAPTURE(r.wants);
        CAPTURE(r.modal);
        CHECK(mouseButtonsReachGame(PolicyState{ r.wants, false, r.modal }) == r.expected);
    }
}

TEST_CASE("input_capture_policy: wheel reaches the game unless the cursor hovers ImGui") {
    struct Row { bool hovers; bool modal; bool expected; };
    const Row rows[] = {
        { false, false, true  },
        { false, true,  true  },
        { true,  false, false },  // hover is spatial: ImGui owns the wheel
        { true,  true,  false },  // ImGui still owns the wheel over a modal
    };
    for (const Row& r : rows) {
        CAPTURE(r.hovers);
        CAPTURE(r.modal);
        CHECK(wheelReachesGame(PolicyState{ false, r.hovers, r.modal }) == r.expected);
        CHECK_FALSE(wheelReachesGame(PolicyState{ true, r.hovers, r.modal }));
    }
}

TEST_CASE("input_capture_policy: press/release edges") {
    // A recorded sample sequence: down, down, up, up, down, up.
    const bool samples[]  = { true,  true,  false, false, true,  false };
    const Edge expected[] = { Edge::Pressed, Edge::Held, Edge::Released, Edge::Free,
                              Edge::Pressed, Edge::Released };

    Edge prev = Edge::Free;
    for (int i = 0; i < 6; ++i) {
        CAPTURE(i);
        prev = nextEdge(prev, samples[i]);
        CHECK(prev == expected[i]);
    }
}

TEST_CASE("input_capture_policy: a release while free stays free") {
    CHECK(nextEdge(Edge::Free,     false) == Edge::Free);
    CHECK(nextEdge(Edge::Released, false) == Edge::Free);
    CHECK(nextEdge(Edge::Held,     true)  == Edge::Held);
    CHECK(nextEdge(Edge::Pressed,  true)  == Edge::Held);
}

TEST_CASE("input_capture_visibility") {
    // Legacy pause buttons win; the wheel remains owned by ImGui.
    struct Frame { bool wants; bool hovers; bool modal; bool buttons; bool wheel; };
    const Frame frames[] = {
        // ImGui window open over the viewport, no modal: ImGui owns both.
        { true,  true,  false, false, false },
        // Pause menu up over the same window: legacy buttons win.
        { true,  true,  true,  true,  false },
        // Menu closed, window still open: back to ImGui.
        { true,  true,  false, false, false },
        // Window closed: the game owns both.
        { false, false, false, true,  true  },
    };
    for (const Frame& f : frames) {
        const PolicyState s{ f.wants, f.hovers, f.modal };
        CAPTURE(f.wants);
        CAPTURE(f.hovers);
        CAPTURE(f.modal);
        CHECK(mouseButtonsReachGame(s) == f.buttons);
        CHECK(wheelReachesGame(s) == f.wheel);
    }
}

TEST_CASE("input_capture_policy: ImGui mouse drag, navigation focus and text field") {
    // GuiRuntime publishes WantTextInput; general WantCaptureKeyboard is true in all three rows.
    struct IO { bool WantCaptureKeyboard; bool WantTextInput; };
    struct Row { const char* action; IO io; bool expected; };
    const Row rows[] = {{"mouse-held window drag", {true, false}, false}, {"focused non-text window", {true, false}, false}, {"active text field", {true, true}, true}};
    for (const auto& row : rows) {
        CAPTURE(row.action);
        bool blocked = false;
        const bool capture = gui_window_policy::capturesKeyboard(row.io);
        CHECK(capture == row.expected);
        CHECK(input::capture::keyboardEdge(input::capture::Edge::Held, true, capture, blocked)
              == (row.expected ? input::capture::Edge::Free : input::capture::Edge::Held));
        CHECK(blocked == row.expected);
    }
}

TEST_CASE("input_capture_policy: dialog keys retain their release during text capture") {
    bool blocked = true;
    auto edge = input::capture::keyboardEdge(Edge::Free, true, true, blocked, true);
    CHECK(edge == Edge::Pressed);
    CHECK_FALSE(blocked);
    edge = input::capture::keyboardEdge(edge, true, true, blocked, true);
    CHECK(edge == Edge::Held);
    edge = input::capture::keyboardEdge(edge, false, true, blocked, true);
    CHECK(edge == Edge::Released);
}

TEST_CASE("input_capture_policy: held game gesture survives a menu opening") {
    PointerCapture pointer;
    CHECK(pointer.sample(1, true) == 1);
    CHECK(pointer.sample(1, true) == 1);
    pointer.claim(); // the menu opens mid-gesture: nothing new was pressed
    CHECK(pointer.sample(1, false) == 1);
    Edge edge = pointer.edge(Edge::Held, 1, pointer.sample(0, false));
    CHECK(edge == Edge::Released);
}

TEST_CASE("input_capture_policy: press while captured is Free through release") {
    PointerCapture pointer;
    CHECK(pointer.sample(1, false) == 0);
    pointer.claim();
    CHECK(pointer.edge(Edge::Free, 1, pointer.sample(1, false)) == Edge::Free);
    CHECK(pointer.edge(Edge::Free, 1, pointer.sample(0, false)) == Edge::Free);
}
