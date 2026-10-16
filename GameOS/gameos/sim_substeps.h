#pragma once
// How many fixed sim steps the main loop runs per rendered frame.
// WHY: the non-visual proofs (sim_determinism, saveload round trip) only need the sim, so under the fixed-step
// harness clock they may run several 1/30 s steps per drawn frame and hold the single game host for a fraction of
// the wall clock. The step size and every RNG draw stay those of 1x; only the number of steps between draws changes.
// Only the harness clock (MC2_SMOKE_FIXED_TIMESTEP) qualifies, only after mission_ready, and only while the window
// lacks input focus, so a player-facing window never runs ahead of its input.

namespace mc2_sim_substeps {

//: Upper bound; matches the clamp of the MC2_SIM_SPEED gate row.
inline constexpr int kMaxStepsPerFrame = 4;

// speedSeed is the MC2_SIM_SPEED value; a fraction rounds down and anything below 2 is one step.
constexpr int stepsPerFrame(float speedSeed, bool fixedTimestep, bool missionStarted, bool windowFocused) {
    if (!fixedTimestep || !missionStarted || windowFocused) return 1;
    if (!(speedSeed >= 2.0f)) return 1;   // NaN lands here too
    const int n = static_cast<int>(speedSeed);
    return n > kMaxStepsPerFrame ? kMaxStepsPerFrame : n;
}

}  // namespace mc2_sim_substeps
