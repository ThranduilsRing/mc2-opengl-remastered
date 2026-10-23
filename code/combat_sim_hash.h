#pragma once
#include <cstdint>
class GameObjectManager;
// Hash authoritative live combat state without importing effect/runtime headers.
std::uint64_t combatSimulationHash(GameObjectManager& objects);
