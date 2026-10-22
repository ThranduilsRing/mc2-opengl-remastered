// WHAT: the range checks for the counts a global move map reads from a .pak, and the zeroed owner helper its buffers use.
// WHY: the counts are mod content. A count out of range must be refused with one log line before it sizes a buffer or
// walks a table, never trusted; the checks live here, free of the engine, so a malformed-pak test can drive each one.
#pragma once

#include <cstddef>
#include <vector>

namespace mc2::globalmap
{
// The design limits of the map builder (move.h MAX_GLOBALMAP_AREAS / MAX_GLOBALMAP_DOORS, terrain.h MAX_MAP_CELL_WIDTH);
// move_internal.h pins them equal with a static_assert.
inline constexpr long kMaxCellsPerSide = 3072;
inline constexpr long kMaxAreas = 30000;
inline bool areaCountInRange(long count) { return count >= 0 && count <= kMaxAreas; }
// DoorInfo/DoorLink store signed 16-bit indices. The two scratch doors
// occupy numDoors and numDoors + 1, so both must remain <= 32767.
inline constexpr long kMaxDoors = 32766;
// The builder refuses an area with more than 255 doors, so a door side links at most 255 other doors plus the start and goal scratch links.
inline constexpr long kMaxDoorsPerArea = 255;
inline constexpr long kDoorScratchLinks = 2;
inline constexpr long kMaxLinksPerDoorSide = kMaxDoorsPerArea + kDoorScratchLinks;
// A mod packet may be larger than the shipped struct (extended schemas); it is accepted up to this many times the expected size,
// plus a fixed floor so a tiny struct still has room for one extra field.
inline constexpr std::size_t kMaxPacketSlack = 16;
inline constexpr std::size_t kPacketSlackFloor = 4096;
// tolerantBytes() result for a packet beyond the slack.
inline constexpr std::size_t kRefused = 0;

struct HeaderCounts
{
	long height = 0;
	long width = 0;
	long sectorHeight = 0;
	long sectorWidth = 0;
	long numAreas = 0;
	long numDoors = 0;
	long numDoorInfos = 0;
	long numDoorLinks = 0;
};

// nullptr when every count is in range, else the name of the first one that is not.
inline const char* rejectHeaderCounts (const HeaderCounts& c)
{
	if (c.height < 1 || c.height > kMaxCellsPerSide)
		return "height";
	if (c.width < 1 || c.width > kMaxCellsPerSide)
		return "width";
	if (c.sectorHeight < 0 || c.sectorHeight > kMaxCellsPerSide)
		return "sectorHeight";
	if (c.sectorWidth < 0 || c.sectorWidth > kMaxCellsPerSide)
		return "sectorWidth";
	if (c.numAreas < 0 || c.numAreas > kMaxAreas)
		return "numAreas";
	if (c.numDoors < 0 || c.numDoors > kMaxDoors)
		return "numDoors";
	// The info and link tables are bounded by this map's own area and door counts, so a header cannot size a table larger
	// than the map it describes.
	if (c.numDoorInfos < 0 || c.numDoorInfos > c.numAreas * kMaxDoorsPerArea)
		return "numDoorInfos";
	if (c.numDoorLinks < 0 || c.numDoorLinks > (c.numDoors + kDoorScratchLinks) * 2 * kMaxLinksPerDoorSide)
		return "numDoorLinks";
	return nullptr;
}

// A per-area door count read from the areas packet: it sizes that area's slice of the door-info table.
inline bool areaDoorCountInRange (long numDoors)
{
	return numDoors >= 0 && numDoors <= kMaxDoorsPerArea;
}

// A per-door-side link count read from the doors packet (before the two scratch links are added).
inline bool doorLinkCountInRange (long numLinks)
{
	return numLinks >= 0 && numLinks <= kMaxDoorsPerArea;
}

// The number of packets one global map occupies in the pak, from its header counts: the 13 fixed packets, the door infos, and two
// per door (links and offsets). A refused map still advances the caller's cursor by this much so the next map is read from the
// right packet; `fallback` is returned when the counts cannot describe a layout (negative), and the result saturates at LONG_MAX.
inline long layoutPacketCount (long numDoorInfos, long numDoors, long doorOffsets, long fallback)
{
	if (numDoorInfos < 0 || numDoors < 0)
		return fallback;
	const long long total = 13LL + numDoorInfos + (static_cast<long long>(numDoors) + doorOffsets) * 2;
	return total > 0x7fffffffLL ? 0x7fffffff : static_cast<long>(total);
}

// The packet index `count` packets after `cursor`, saturating at LONG_MAX instead of overflowing: a hostile layout size from a
// refused map must not wrap the next read position, and an index that large is past the end of any pak so the next read fails.
inline long advanceCursor (long cursor, long count)
{
	const long long next = static_cast<long long>(cursor) + count;
	return next > 0x7fffffffLL ? 0x7fffffff : (next < 0 ? 0 : static_cast<long>(next));
}

// The buffer size for a packet that may be larger than the struct array the reader expects: kRefused when the packet is more than
// kMaxPacketSlack times the expected size plus the floor, else the larger of the two.
inline std::size_t tolerantBytes (std::size_t packetBytes, std::size_t expectedBytes)
{
	if (packetBytes > expectedBytes * kMaxPacketSlack + kPacketSlackFloor)
		return kRefused;
	return packetBytes > expectedBytes ? packetBytes : expectedBytes;
}

// Replaces the contents of `store` with `count` zeroed elements (at least one, so the returned view is never null) and
// returns the view. A view taken from the store is valid until the next call on it or the store's destruction.
template <class T>
T* ownZeroed (std::vector<T>& store, std::size_t count)
{
	store.assign(count ? count : 1, T{});
	return store.data();
}
}
