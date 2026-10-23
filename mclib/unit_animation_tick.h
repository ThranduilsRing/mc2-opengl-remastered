#pragma once
namespace mc2::sim {
struct UnitAnimState;
// The linear clip clock shared by buildings, vehicles and gameplay VTOLs.
void advanceLinearAnimation(UnitAnimState& state, float rate, long frames, float seconds);
}
