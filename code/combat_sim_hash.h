#pragma once
#include <cstdint>
#include "save_archive.h"
template<class MoverState>
void combatMoverFields(mc2::save::Archive& state, MoverState& mover) {
    state(mover.numWeapons, mover.numAmmos, mover.numAmmoTypes);
    for (int w = mover.numOther; w < mover.numOther + mover.numWeapons; ++w)
        state(mover.inventory[w].readyTime, mover.inventory[w].amount);
    for (int a = mover.numOther + mover.numWeapons; a < mover.numOther + mover.numWeapons + mover.numAmmos; ++a)
        state(mover.inventory[a].amount);
    for (int a = 0; a < mover.numAmmoTypes; ++a)
        state(mover.ammoTypeTotal[a].curAmount);
}
class GameObjectManager;
// Hash authoritative live combat state without importing effect/runtime headers.
std::uint64_t combatSimulationHash(GameObjectManager& objects);
