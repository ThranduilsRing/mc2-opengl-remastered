// WHAT: the range checks for the counts a global move map reads from a .pak, and the zeroed owner helper its buffers use.
// WHY: the counts are mod content. A count out of range must be refused with one log line before it sizes a buffer or
// walks a table, never trusted; the checks live here, free of the engine, so a malformed-pak test can drive each one.
#pragma once

#include <cstddef>
#include <vector>

namespace mc2::globalmap
{
// The design limits of the map builder (move.h MAX_GLOBALMAP_AREAS / MAX_GLOBALMAP_DOORS, terrain.h MAX_MAP_CELL_WIDTH);
// move.cpp pins them equal with static_asserts.
inline constexpr long kMaxCellsPerSide = 720;
inline constexpr long kMaxAreas = 10000;
inline constexpr long kMaxDoors = 10000;
// The builder refuses an area with more than 255 doors, so a door side links at most 255 other doors plus two scratch links.
inline constexpr long kMaxDoorsPerArea = 255;
inline constexpr long kMaxLinksPerDoorSide = kMaxDoorsPerArea + 2;
// A mod packet may be larger than the shipped struct (extended schemas); it is accepted up to this many times the expected size.
inline constexpr std::size_t kMaxPacketSlack = 16;

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
	if (c.numDoorInfos < 0 || c.numDoorInfos > kMaxAreas * kMaxDoorsPerArea)
		return "numDoorInfos";
	if (c.numDoorLinks < 0 || c.numDoorLinks > (kMaxDoors + 2) * 2 * kMaxLinksPerDoorSide)
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

// The buffer size for a packet that may be larger than the struct array the reader expects: 0 when the packet is more than
// kMaxPacketSlack times the expected size (a refusal), else the larger of the two.
inline std::size_t tolerantBytes (std::size_t packetBytes, std::size_t expectedBytes)
{
	if (packetBytes > expectedBytes * kMaxPacketSlack + 4096)
		return 0;
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
