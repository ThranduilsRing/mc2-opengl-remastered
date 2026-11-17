#pragma once
// Which overlay art tile a terrain overlay handle selects, and whether the transition bake combines it.
// The mission only needs the tiles its transitions combine; the loader reads and hashes those on first use.
#include <cstddef>
#include <span>

namespace mc2 {
struct OverlayTileRef { long overlay = 0; long tile = 0; };

// base[i] = first handle of overlay i, count[i] = its tile count. A handle in no range keeps the legacy
// fallback (overlay 0, tile 0): combineOverlayTxm has always read that tile for it.
inline OverlayTileRef overlayTileForHandle(std::span<const long> base, std::span<const long> count, long handle)
{
	for (size_t i = 0; i < base.size() && i < count.size(); ++i)
		if (handle >= base[i] && handle < base[i] + count[i])
			return { static_cast<long>(i), handle - base[i] };
	return {};
}

// The createTransition rule: 0xffff means no overlay; with the colour map, handles 285..292 are skipped.
inline bool overlayHandleCombined(unsigned long handle, bool hasColorMap)
{
	if (handle == 0xffff) return false;
	return !hasColorMap || handle < 285 || handle > 292;
}
}
