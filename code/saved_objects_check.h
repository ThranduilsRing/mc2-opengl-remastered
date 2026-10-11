//============================================================================
// saved_objects_check.h -- structural check of the object packets of an in-mission save.
//
// WHAT: a save stores one packet per object slot; GameObjectManager::Load tells the object class
// from the packet's byte size and sizes its per-class arrays from the header counts. This header is
// the pure rule for "does the packet run agree with its header": every slot's size is empty or one
// known class size, the per-class totals equal the header counts, and the run is complete.
// WHY: a save written by an older build has different struct sizes, so a class is not recognised and
// its array stays unfilled; the loader then dereferenced the empty slots and crashed. The check runs
// BEFORE the running mission is torn down so a bad save is refused and the game carries on.
// Header-only, no engine types, so the doctest can drive it with synthetic sizes.
//============================================================================
#ifndef SAVED_OBJECTS_CHECK_H
#define SAVED_OBJECTS_CHECK_H

#include <cstdint>

namespace mc2_saved_objects {

// Order matters: the loader tests the sizes in this order and the first match wins.
enum ObjectClass : int {
	kTerrain, kBuilding, kTurret, kGate, kArtillery, kCarnage, kMech, kVehicle, kBolt,
	kClassCount
};

// sizeof() of each packed object record of the running build, plus the two base records that must
// never be saved on their own.
struct RecordSizes {
	uint32_t byClass[kClassCount];
	uint32_t pureObject;
	uint32_t pureMover;
};

// The counts of the saved header. Elementals and lights have no loader branch in the engine, so a
// save that holds any is refused instead of being half read.
struct HeaderCounts {
	int32_t byClass[kClassCount];
	int32_t elementals;
	int32_t lights;
};

// A header counts as a candidate only when every count is non-negative and the slot total is sane.
// A 60-byte packet elsewhere in the file must not be mistaken for the header.
constexpr int64_t kMaxSlots = 1 << 20;

inline int64_t slotCount(const HeaderCounts& h)
{
	int64_t total = static_cast<int64_t>(h.elementals) + h.lights;
	for (int c = 0; c < kClassCount; ++c)
		total += h.byClass[c];
	return total;
}

inline bool plausibleHeader(const HeaderCounts& h)
{
	if (h.elementals < 0 || h.lights < 0)
		return false;
	for (int c = 0; c < kClassCount; ++c)
		if (h.byClass[c] < 0)
			return false;
	const int64_t total = slotCount(h);
	return total > 0 && total < kMaxSlots;
}

// The class of a packet of `size` bytes, -1 for an unrecognised size, -2 for a base record.
inline int classOfSize(const RecordSizes& r, uint32_t size)
{
	if (size == r.pureObject || size == r.pureMover)
		return -2;
	for (int c = 0; c < kClassCount; ++c)
		if (size == r.byClass[c])
			return c;
	return -1;
}

// Check the run that follows the header at packet `headerIndex`: slots 0..maxObjects, then the watch
// table of maxObjects+1 ids. `sizeAt(i)` is the byte size of packet i. -> nullptr when the run is
// sound, else a short reason tag. `badPacket` receives the offending packet index (or -1).
template <class SizeAt>
inline const char* checkRun(const HeaderCounts& h, const RecordSizes& r, uint32_t headerIndex,
                            uint32_t numPackets, SizeAt sizeAt, int64_t* badPacket = nullptr)
{
	if (badPacket)
		*badPacket = -1;
	if (h.elementals != 0 || h.lights != 0)
		return "unsupported_object_class";
	const int64_t slots = slotCount(h) + 1;
	const int64_t first = static_cast<int64_t>(headerIndex) + 1;
	if (first + slots + 1 > static_cast<int64_t>(numPackets))
		return "truncated";

	int64_t seen[kClassCount] = {};
	for (int64_t i = 0; i < slots; ++i) {
		const uint32_t size = sizeAt(static_cast<uint32_t>(first + i));
		if (size == 0)
			continue;
		const int c = classOfSize(r, size);
		if (c == -2) {
			if (badPacket)
				*badPacket = first + i;
			return "base_record_saved";
		}
		if (c < 0) {
			if (badPacket)
				*badPacket = first + i;
			return "unknown_record_size";
		}
		++seen[c];
	}
	for (int c = 0; c < kClassCount; ++c)
		if (seen[c] != h.byClass[c])
			return "object_count_mismatch";

	if (sizeAt(static_cast<uint32_t>(first + slots)) != static_cast<uint32_t>(sizeof(int32_t) * slots))
		return "bad_watch_table";
	return nullptr;
}

} // namespace mc2_saved_objects

#endif
