// SCREEN-SETTLE-SHOT-1: the "screenshot after screen X has rendered N settled frames, then exit" trigger state machine (header-only).
#include "doctest.h"

#include "screen_settle_capture.h"

#include <string>

using namespace mc2::screen_settle;
using Action = Trigger::Action;
using State = Trigger::State;

TEST_CASE("screen settle: spec parsing") {
    Spec s;
    std::string why;
    CHECK(parseSpec("mechbay", s, &why));
    CHECK(s.settleFrames == 30);
    CHECK(s.cell->x == 2);
    CHECK(s.cell->y == 1);

    CHECK(parseSpec("Campaign_Missions@12", s, &why));   // ids are case-insensitive
    CHECK(s.settleFrames == 12);
    CHECK(s.cell->x == 0);
    CHECK(s.cell->y == 1);

    CHECK(parseSpec("logistics", s, &why));   // alias of the briefing screen (the one with the tactical minimap)
    CHECK(s.cell->x == findScreen("briefing")->x);
    CHECK(s.cell->y == findScreen("briefing")->y);

    CHECK_FALSE(parseSpec("", s, &why));
    CHECK_FALSE(parseSpec("nowhere", s, &why));
    CHECK(why == "unknown screen id");
    CHECK_FALSE(parseSpec("mechbay@", s, &why));
    CHECK_FALSE(parseSpec("mechbay@0", s, &why));
    CHECK_FALSE(parseSpec("mechbay@x3", s, &why));
    CHECK_FALSE(parseSpec("mechbay@9999999", s, &why));
}

TEST_CASE("screen settle: captures after N consecutive frames on the screen") {
    Trigger t;
    CHECK_FALSE(t.armed());
    CHECK(t.onFrame(2, 1) == Action::None);   // Off: ignores everything

    t.arm(2, 1, 3, 100);
    CHECK(t.state() == State::Waiting);
    CHECK(t.onFrame(-1, -1) == Action::None);   // main menu
    CHECK(t.onFrame(0, 1) == Action::None);     // other screen
    CHECK(t.onFrame(2, 1) == Action::None);
    CHECK(t.state() == State::Settling);
    CHECK(t.onFrame(2, 1) == Action::None);
    CHECK(t.onFrame(2, 1) == Action::Capture);
    CHECK(t.state() == State::Requested);
    CHECK(t.onFrame(2, 1) == Action::None);     // one capture only
    t.onCaptureResult(true);
    CHECK(t.state() == State::Done);
    CHECK(t.finished());
}

TEST_CASE("screen settle: leaving the screen restarts the settle count") {
    Trigger t;
    t.arm(2, 1, 3, 100);
    CHECK(t.onFrame(2, 1) == Action::None);
    CHECK(t.onFrame(2, 1) == Action::None);
    CHECK(t.onFrame(3, 1) == Action::None);   // transition frame
    CHECK(t.settledFrames() == 0);
    CHECK(t.onFrame(2, 1) == Action::None);
    CHECK(t.onFrame(2, 1) == Action::None);
    CHECK(t.onFrame(2, 1) == Action::Capture);
}

TEST_CASE("screen settle: a screen never reached fails with a reason") {
    Trigger t;
    t.arm(2, 1, 3, 5);
    for (int i = 0; i < 4; ++i) CHECK(t.onFrame(0, 1) == Action::None);
    CHECK(t.onFrame(0, 1) == Action::Fail);
    CHECK(t.state() == State::Failed);
    CHECK(std::string(t.reason()).find("never reached") != std::string::npos);
    CHECK(t.onFrame(2, 1) == Action::None);   // a failed trigger stays failed
}

TEST_CASE("screen settle: the mission starting first is a failure; a finished trigger ignores it") {
    Trigger t;
    CHECK(t.onLeftFrontEnd() == Action::None);   // Off
    t.arm(2, 1, 3, 100);
    CHECK(t.onLeftFrontEnd() == Action::Fail);
    CHECK(std::string(t.reason()).find("front end ended") != std::string::npos);

    Trigger d;
    d.arm(2, 1, 1, 100);
    CHECK(d.onFrame(2, 1) == Action::Capture);
    d.onCaptureResult(true);
    CHECK(d.onLeftFrontEnd() == Action::None);   // already Done
    CHECK(d.state() == State::Done);
}

TEST_CASE("screen settle: ImGui not ready is a loud failure, never a (black) capture") {
    Trigger t;
    CHECK(t.onImguiNotReady() == Action::None);   // Off
    t.arm(2, 1, 1, 100);
    CHECK(t.onFrame(2, 1) == Action::Capture);    // the engine hook now checks GuiRuntime::VulkanReady() ...
    CHECK(t.onImguiNotReady() == Action::Fail);   // ... and reports this instead of requesting the screenshot
    CHECK(t.state() == State::Failed);
    CHECK(std::string(t.reason()).find("imgui_vk_ready=0") != std::string::npos);
}

TEST_CASE("screen settle: a failed screenshot write is a failure") {
    Trigger t;
    t.arm(2, 1, 1, 100);
    CHECK(t.onFrame(2, 1) == Action::Capture);
    t.onCaptureResult(false);
    CHECK(t.state() == State::Failed);
    CHECK(std::string(t.reason()).find("write failed") != std::string::npos);
}
