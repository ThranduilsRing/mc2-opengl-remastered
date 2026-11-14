#pragma once
// The fixed simulation tick: 1/30 s of sim time per tick, however fast the frames come.
// WHY: the sim used to advance once per rendered frame by that frame's measured length, so its result depended on frame
// rate (and on the MC2_SIM_SPEED substeps hack). Now the render loop banks real elapsed time x game speed in this
// accumulator and runs whole ticks out of it. Game speed changes how OFTEN ticks run, never how long one tick is, so the
// same mission and input give the same per-tick results at 30, 60 or 240 fps and at 0.25x or 4x.

#include <algorithm>
#include <bit>
#include <cmath>

namespace mc2::sim {

//: Sim seconds one tick advances (scenarioTime, frameLength, every animation and timer step in the tick).
inline constexpr double kTickSeconds = 1.0 / 30.0;
//: The same tick as a float, bit-identical to the legacy harness step SmokeMode::fixedTimestepMs()/1000.0f (0x3D088888, one ulp
//: below float(1.0/30.0)): float timers subtract it per tick, so a different ulp expires round timers a tick early.
//: Every float use of the tick (scenarioTime +=, frameLength) must use this one constant.
inline constexpr float kTickSecondsF = 1000.0f / 30.0f / 1000.0f;
static_assert(std::bit_cast<unsigned>(kTickSecondsF) == 0x3D088888u, "legacy tick bit pattern");
//: Retain at most one wall second of debt; longer interruptions discard only the excess.
inline constexpr double kMaxRealFrameSeconds = 1.0;
//: Ticks per frame at 1x; the cap scales with game speed so fast-forward is never clamped below its own rate.
inline constexpr int kBaseMaxTicksPerFrame = 30;

inline constexpr int kBridgeMaxStepsPerFrame = 4;
// A tick cannot be interrupted; yield between whole ticks when the frame budget is spent.
inline constexpr double kCatchUpBudgetSeconds = 0.008;
constexpr bool catchUpSpent(double elapsedSeconds) { return elapsedSeconds >= kCatchUpBudgetSeconds; }

inline int maxTicksPerFrame(double speed) {
    if (!(speed > 1.0)) return kBaseMaxTicksPerFrame;   // NaN lands here too
    return static_cast<int>(std::ceil(kBaseMaxTicksPerFrame * speed));
}

class TickClock {
public:
    // Bank realSeconds x speed and return how many whole ticks to run now. Leftover time stays banked.
    int advance(double realSeconds, double speed) {
        if (!(realSeconds > 0.0) || !(speed > 0.0)) return 0;
        realSeconds = std::min(realSeconds, kMaxRealFrameSeconds);
        banked_ = std::min(banked_ + realSeconds * speed, kMaxRealFrameSeconds * speed);
        int ticks = static_cast<int>(std::floor(banked_ / kTickSeconds + 1e-6));
        const int cap = maxTicksPerFrame(speed);
        ticks = std::min(ticks, cap);
        banked_ = std::max(0.0, banked_ - ticks * kTickSeconds);
        return ticks;
    }
    void reset() { banked_ = 0.0; }
    // advance reserves a batch; return ticks deferred by the wall-time budget without losing their phase.
    void defer(int ticks) { banked_ += ticks * kTickSeconds; }
    // Fraction of the next tick already elapsed, 0..1: the render interpolation weight.
    double alpha() const { return std::min(1.0, banked_ / kTickSeconds); }

private:
    double banked_ = 0.0;
};

}  // namespace mc2::sim
