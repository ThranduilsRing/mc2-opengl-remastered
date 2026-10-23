#pragma once
// The appearance-side random stream.
//
// WHAT: the random numbers that appearance and effect code draws: a destroyed mech's fall direction, the first frame of a
// looping animation, the rotation of a crater decal, the tumble of a blown-off arm, the wiggle of a beam. Seeded once per
// mission from the mission seed, so a run is still replayable.
// WHY: how often appearance code runs can depend on the view (culling, the effects option, a paused appearance-only update),
// so its draws must never advance the stream the simulation rolls on (gos_rand): a camera move would otherwise change a
// to-hit roll. Weather already has its own stream for the same reason (code/weather.cpp). Simulation code keeps RandomNumber
// and RollDice; appearance code uses these.
#include <cstdint>

namespace mc2_appearance_rng {

// Restart the stream; Mission::init calls this with the mission seed.
void reseed(uint32_t seed);

// Like RandomNumber: a value in [0, range); 0 when range is not positive.
long randomNumber(long range);

// Like RollDice: true with the given percent chance.
bool rollDice(long percent);

// The number of draws since the last reseed, for the [DT] determinism trace and the SIMID digest.
uint32_t drawCount();

} // namespace mc2_appearance_rng

// Gameplay animation decisions have their own stream. Presentation draws must
// never change a death gesture, gate transition, or locomotion clock.
namespace mc2_anim_rng {
void reseed(uint32_t seed);
long randomNumber(long range);
bool rollDice(long percent);
uint32_t drawCount();
uint32_t state();
void restore(uint32_t state, uint32_t draws);
}
