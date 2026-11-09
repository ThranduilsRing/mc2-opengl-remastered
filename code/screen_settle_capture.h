#pragma once
// SCREEN-SETTLE-SHOT-1: "screenshot after screen X has rendered N settled frames, then exit".
// Proof/test tool, not a feature: MC2_SHOT_SCREEN=<screen id>[@<settle frames>] (docs/tier1_env_vars.md).
// Header-only and engine-free so a doctest pins the state machine (tests/unit/test_screen_settle_capture.cpp).
#include <cctype>
#include <string>
#include <string_view>

namespace mc2::screen_settle {

// Logistics front-end grid cells (MissionBegin::screens[x][y]); the first name of a cell is its canonical id.
struct ScreenCell { const char* name; int x; int y; };
inline constexpr ScreenCell kScreens[] = {
    {"campaign_missions", 0, 1},  // mission selection (campaign / globe screen)
    {"logistics", 1, 1},          // alias of briefing: the first logistics-stage screen, with the tactical minimap
    {"briefing", 1, 1},
    {"purchase", 2, 0},
    {"mechbay", 2, 1},
    {"bay", 2, 1},
    {"loadout", 2, 2},
    {"pilots", 3, 1},
    {"launch", 3, 1},
    {"load", 4, 1},
};

inline bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}

inline const ScreenCell* findScreen(std::string_view id) {
    for (const ScreenCell& c : kScreens)
        if (iequals(id, c.name)) return &c;
    return nullptr;
}

struct Spec {
    std::string screen;       // as written
    const ScreenCell* cell = nullptr;
    int settleFrames = 30;
};

// "<id>" or "<id>@<N>" (N >= 1). False (with `why`) for an empty value, unknown id or bad N.
inline bool parseSpec(std::string_view text, Spec& out, std::string* why = nullptr) {
    auto fail = [&](const char* m) { if (why) *why = m; return false; };
    if (text.empty()) return fail("empty");
    const size_t at = text.find('@');
    out.screen = std::string(text.substr(0, at));
    out.settleFrames = 30;
    if (at != std::string_view::npos) {
        const std::string_view n = text.substr(at + 1);
        if (n.empty() || n.size() > 6) return fail("bad settle frame count");
        int v = 0;
        for (char c : n) {
            if (c < '0' || c > '9') return fail("bad settle frame count");
            v = v * 10 + (c - '0');
        }
        if (v < 1) return fail("settle frame count must be >= 1");
        out.settleFrames = v;
    }
    out.cell = findScreen(out.screen);
    if (!out.cell) return fail("unknown screen id");
    return true;
}

// One call per front-end frame the target could be on. Screen reached = the grid cell equals the target; the settle count restarts if
// the screen is left. A screen never reached within maxWaitFrames, or leaving the front end (mission launched) first, is a Fail.
class Trigger {
public:
    enum class State { Off, Waiting, Settling, Requested, Done, Failed };
    enum class Action { None, Capture, Fail };

    void arm(int cellX, int cellY, int settleFrames, int maxWaitFrames) {
        x_ = cellX; y_ = cellY; settle_ = settleFrames; maxWait_ = maxWaitFrames;
        settled_ = 0; frames_ = 0; state_ = State::Waiting; reason_ = "";
    }
    bool armed() const { return state_ != State::Off; }
    State state() const { return state_; }
    int settledFrames() const { return settled_; }
    int framesSeen() const { return frames_; }
    const char* reason() const { return reason_; }
    bool finished() const { return state_ == State::Done || state_ == State::Failed; }

    Action onFrame(int curX, int curY) {
        if (state_ == State::Off || state_ == State::Requested || finished()) return Action::None;
        ++frames_;
        if (curX == x_ && curY == y_) {
            state_ = State::Settling;
            if (++settled_ >= settle_) { state_ = State::Requested; return Action::Capture; }
            return Action::None;
        }
        settled_ = 0;
        state_ = State::Waiting;
        if (frames_ >= maxWait_) return fail("screen never reached within the frame budget");
        return Action::None;
    }
    // The mission (or anything past the front end) started before the screenshot was written.
    Action onLeftFrontEnd() {
        if (state_ == State::Off || finished()) return Action::None;
        return fail("front end ended before the screen was captured");
    }
    // Checked when the settle count is met (Action::Capture): the ImGui renderer is not up, so the front-end frame would be black.
    // Never save that frame; fail loudly instead.
    Action onImguiNotReady() {
        if (state_ == State::Off || finished()) return Action::None;
        return fail("ImGui not ready (imgui_vk_ready=0): the frame would be black");
    }
    // The screenshot request finished (file written) or failed.
    void onCaptureResult(bool ok) {
        if (state_ != State::Requested) return;
        if (ok) state_ = State::Done; else fail("screenshot write failed");
    }

private:
    Action fail(const char* why) { state_ = State::Failed; reason_ = why; return Action::Fail; }
    int x_ = 0, y_ = 0, settle_ = 0, maxWait_ = 0, settled_ = 0, frames_ = 0;
    State state_ = State::Off;
    const char* reason_ = "";
};

// Engine hooks (code/missionbegin.cpp). The engine-free state machine above is what the doctest pins.
void tickFrontEnd(int curX, int curY);   // MissionBegin::render, once per front-end frame (-1,-1 while the main menu/splash is up)
void onMissionStarted();                 // logistics finished (mission launching): a still-pending capture fails

}  // namespace mc2::screen_settle
