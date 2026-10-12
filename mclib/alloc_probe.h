#pragma once
// Allocation accounting seam.
//
// WHAT: one portable pair of calls, allocate / release, that counts live allocations and live bytes (each block carries its size in a
// header, so no platform query such as _msize is needed) and a snapshot of the counters.
// WHY: the ownership step needs a leak oracle that is not the exit-time [ALLOC_STATS] ratchet: a mission reload must bring the live
// figure back to where the first load left it, and a doctest must leave the heap where it found it (ScopedLeakCheck in the unit tier,
// whose allocator routes every new/delete through here). Proof builds only in the game: the engine's gos_Malloc is wired to it by the
// batch that needs the reload delta; nothing in a default build calls it.
#include <cstddef>
#include <cstdint>

namespace mc2_alloc_probe {

struct Snapshot {
	uint64_t live;    // blocks allocated and not yet released
	uint64_t bytes;   // their sizes as requested
	uint64_t total;   // allocations since start, for "did anything allocate at all"
};

// malloc with accounting; nullptr when malloc fails. A zero-byte request still returns a distinct block.
void* allocate(size_t bytes);

// Frees an allocate() result; nullptr is a no-op. Releasing a block that did not come from allocate() is a bug it cannot detect.
void release(void* p);

Snapshot snapshot();

} // namespace mc2_alloc_probe
