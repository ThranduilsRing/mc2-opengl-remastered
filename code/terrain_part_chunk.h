// Terrain part ids in the 20-bit chunk fields (weapon fire/hit, status, tactical order).
//
// A terrain part id is MIN_TERRAIN_PART_ID + row * gTerrainPartIdStride + col. The chunk fields hold 20 bits, which fits
// every map up to 1024 cells a side exactly (stock stride 720: unchanged, the field is id - MIN_TERRAIN_PART_ID). A larger
// map (stride > 1024) cannot name its cells in 20 bits, so there the field carries a compact index handed out by a
// registry the first time an id is packed; unpack maps it back. Chunks are in-process only for such maps, so the registry
// is the same on both sides of every pack/unpack.
#pragma once
#include "unitdesg.h"
#include "../mclib/terrain.h"
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace terrain_part_chunk {
inline constexpr unsigned long kFieldMax = 0x000FFFFFul;

struct Registry {
	std::unordered_map<unsigned long, unsigned long> forward;
	std::vector<unsigned long> reverse;
};
inline Registry& registry () { static Registry r; return r; }
inline bool compact () { return gTerrainPartIdStride > 1024; }

// id -> 20-bit field value
inline unsigned long pack (long partId) {
	const unsigned long off = static_cast<unsigned long>(partId - MIN_TERRAIN_PART_ID);
	if (!compact())
		return off;
	Registry& r = registry();
	const auto it = r.forward.find(off);
	if (it != r.forward.end())
		return it->second;
	const unsigned long idx = r.reverse.size();
	if (idx > kFieldMax)
		return kFieldMax;   // more than ~1M distinct targeted terrain cells in one mission: saturate
	r.forward.emplace(off, idx);
	r.reverse.push_back(off);
	return idx;
}

// 20-bit field value -> id
inline long unpack (unsigned long field) {
	if (!compact())
		return static_cast<long>(MIN_TERRAIN_PART_ID + field);
	const Registry& r = registry();
	if (field >= r.reverse.size())
		return -1;
	return static_cast<long>(MIN_TERRAIN_PART_ID + r.reverse[field]);
}
}
