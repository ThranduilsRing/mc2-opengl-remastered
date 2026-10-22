#pragma once
#include <cstdint>
// Exact deterministic platform stream state, for checkpoints and replay identity.
std::uint32_t mc2GetSimRngState();
void mc2RestoreSimRngState(std::uint32_t);
void RestoreSimRandomDraws(unsigned long draws);
