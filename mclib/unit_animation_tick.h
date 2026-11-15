#pragma once
namespace mc2::sim {
struct UnitAnimState;
// Presentation still refreshes transforms while paused, but its visual clocks must stop.
constexpr float presentationAnimationSeconds(float seconds, bool frozen) {
    return frozen ? 0.0f : seconds;
}
// The linear clip clock shared by buildings, vehicles and gameplay VTOLs.
void advanceLinearAnimation(UnitAnimState& state, float rate, long frames, float seconds);
}
