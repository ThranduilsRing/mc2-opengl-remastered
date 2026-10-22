#pragma once
// The fixed simulation tick: 1/30 s of sim time per tick, however fast the frames come.
// WHY: the sim used to advance once per rendered frame by that frame's measured length, so its result depended on frame
// rate (and on the MC2_SIM_SPEED substeps hack). Now the render loop banks real elapsed time x game speed in this
// accumulator and runs whole ticks out of it. Game speed changes how OFTEN ticks run, never how long one tick is, so the
// same mission and input give the same per-tick results at 30, 60 or 240 fps and at 0.25x or 4x.

#include <algorithm>
#include <cmath>

namespace mc2::sim {

//: Sim seconds one tick advances (scenarioTime, frameLength, every animation and timer step in the tick).
inline constexpr double kTickSeconds = 1.0 / 30.0;
//: A real frame longer than this is a hitch (load, drag, breakpoint); the excess is dropped rather than replayed.
inline constexpr double kMaxRealFrameSeconds = 0.25;
//: Ticks per frame at 1x; the cap scales with game speed so fast-forward is never clamped below its own rate.
inline constexpr int kBaseMaxTicksPerFrame = 4;

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
        banked_ += realSeconds * speed;
        int ticks = static_cast<int>(std::floor(banked_ / kTickSeconds + 1e-6));
        const int cap = maxTicksPerFrame(speed);
        if (ticks > cap) {
            ticks = cap;
            banked_ = std::fmod(banked_, kTickSeconds);   // drop the backlog, keep the sub-tick phase
        }
        banked_ = std::max(0.0, banked_ - ticks * kTickSeconds);
        return ticks;
    }
    void reset() { banked_ = 0.0; }
    // Fraction of the next tick already elapsed, 0..1: the render interpolation weight.
    double alpha() const { return std::min(1.0, banked_ / kTickSeconds); }

private:
    double banked_ = 0.0;
};

}  // namespace mc2::sim
