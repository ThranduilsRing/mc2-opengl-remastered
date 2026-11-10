// CAPTURE-LIVE-CLOCK: pure sequencing for the bookmark sweep's optional live settle.
// pose applied -> sim runs for liveSec sim-seconds -> sim paused -> normal settle + capture.
// Header-only so tests/unit/test_visual_capture_live_settle.cpp pins it without the engine.
#pragma once

namespace mc2::visual_capture {

enum class LiveSettleStep { Run, Freeze };

// simNow/simStart are scenarioTime values; framesRun counts frames since the sim was unpaused.
// A stalled clock (scenarioTime not advancing) is cut off after maxFrames so the sweep cannot hang.
constexpr LiveSettleStep liveSettleStep(double simNow, double simStart, double liveSec,
                                        int framesRun, int maxFrames) {
    if (liveSec <= 0.0) return LiveSettleStep::Freeze;
    if (simNow - simStart >= liveSec) return LiveSettleStep::Freeze;
    if (framesRun >= maxFrames) return LiveSettleStep::Freeze;
    return LiveSettleStep::Run;
}

constexpr int liveSettleMaxFrames(double liveSec) {
    return static_cast<int>(liveSec * 30.0 * 4.0) + 600;
}

} // namespace mc2::visual_capture
