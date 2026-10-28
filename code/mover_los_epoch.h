// Epoch LOS cache cells (M5): cell = (epoch << 2) | state; state 0 unknown, 1 blocked, 2 clear.
// Advancing the epoch invalidates every cell without clearing the table.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace mc2 {

inline constexpr uint16_t kLosEpochLimit = 1u << 14;

inline void losEpochAdvance(uint16_t* cells, size_t count, uint16_t& epoch)
{
	if (++epoch >= kLosEpochLimit) {
		std::memset(cells, 0, sizeof(uint16_t) * count);
		epoch = 1;
	}
}

inline uint16_t losEpochCell(uint16_t epoch, bool clear) { return (uint16_t)((epoch << 2) | (clear ? 2 : 1)); }

// 0 unknown (stale epoch or never written), 1 blocked, 2 clear.
inline int losEpochState(uint16_t cell, uint16_t epoch) { return (cell >> 2) == epoch ? (cell & 3) : 0; }

} // namespace mc2
