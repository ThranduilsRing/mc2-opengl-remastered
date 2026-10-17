// B7o.4: the queued-waypoint-marker protocol of MechWarrior, header-only so tests/unit/test_warrior_queue_markers.cpp exercises the
// production code (MechWarrior itself cannot be linked into mc2_tests).
//   wire record : QueuedTacOrder, UNCHANGED; its `marker` field is dead storage, null on the wire and in the queue
//   owners      : MechWarrior::queueMarkers, index-parallel to the queue slice; a marker exists only for slot < count
// SAVE-BYTE CONTRACT: no helper below writes a record the legacy code did not write. The shift keeps the legacy slice-capacity bound
// (the stale neighbour at `oldCount` still lands in slot oldCount-1); the ONE deliberate deviation is the full-capacity dequeue, whose
// legacy iteration copied slot 16 (outside the warrior's slice) into slot 15: it now leaves slot 15 as it was.
#pragma once

#include <array>
#include <cstddef>
#include <memory>
#include <utility>

namespace mc2 {

template <class Marker, std::size_t N>
using QueueMarkerArray = std::array<std::unique_ptr<Marker>, N>;

// The only code that clears the slots wholesale (clear, restore, rebind, destroy).
template <class Marker, std::size_t N>
inline void resetQueueMarkers (QueueMarkerArray<Marker, N>& markers) noexcept
{
	for (auto& m : markers)
		m.reset();
}

// Dequeue (oldCount >= 1): the head marker is deleted on purpose, then records and owners shift down. Records follow the legacy loop
// except where it would read slot N (outside the slice); owners move, never copy. The tail owner is null by the invariant.
template <class Rec, class Marker, std::size_t N>
inline void shiftQueueDown (Rec* records, QueueMarkerArray<Marker, N>& markers, long oldCount)
{
	markers[0].reset();
	for (long i = 0; i < oldCount; i++) {
		if (static_cast<std::size_t>(i) + 1 >= N)
			break;
		records[i] = records[i + 1];
		markers[i] = std::move(markers[i + 1]);
	}
}

// Rebind to the next free slice. Capacity is tested FIRST: on failure nothing is touched (records, count, pointer and every owner stay).
// On success the previous binding's markers are released (legacy leaked them) and the new slice's records are NOT written.
template <class Rec, class Marker, std::size_t N, class Pos, class Count>
inline bool rebindQueue (Rec*& queue, Rec* pool, long poolCapacity, Pos& pos, Count& count, QueueMarkerArray<Marker, N>& markers)
{
	if ((poolCapacity - static_cast<long>(pos)) < static_cast<long>(N))
		return false;
	resetQueueMarkers(markers);
	queue = &pool[pos];
	pos += static_cast<Pos>(N);
	count = 0;
	return true;
}

// Restore: each live marker of the previous run is released exactly once, then owners are rebuilt for slot < count only.
// make(record) returns the owner for that record (null for a move mode without a marker).
template <class Rec, class Marker, std::size_t N, class Make>
inline void restoreQueueMarkers (const Rec* records, QueueMarkerArray<Marker, N>& markers, long count, Make&& make)
{
	for (std::size_t i = 0; i < N; i++) {
		markers[i].reset();
		if (static_cast<long>(i) >= count)
			continue;
		markers[i] = make(records[i]);
	}
}

} // namespace mc2
