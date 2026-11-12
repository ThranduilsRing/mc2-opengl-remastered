//***************************************************************************
//
//	path_trace.h -- PATHFINDING-FACTS-1 Stage 1 telemetry helper.
//
//	Additive, gated, default-OFF. When MC2_DIAG_TAGS lacks "PATH" the
//	mc2_diag::tagEnabled("PATH") check is a cheap whitelist lookup that
//	returns false, so the entire trace is skipped: no snprintf, no I/O.
//	The only OFF-path cost is one cached bool + predicted branches.
//
//	HARD RULE: measure only. Nothing here changes pathfinding behavior or
//	raises any constant.
//
//***************************************************************************
#pragma once

#include "mc2_czstring.h"
#include "../GameOS/gameos/diagnostic_trace.h"
#include <cstdio>
#include <chrono>
#include <format>
#include "open_heap.h"
#include "move.h"

class MovePath;

namespace mc2_path_trace {

// FNV-1a over a solved path (steps, cost, goal, target); defined in move_movemap_path.cpp. The ordered "solve" events carry it, so two
// builds that solve the same requests produce the same transcript (the `us` field is timing and is stripped when diffing).
unsigned long long hashMovePath(const MovePath* path);

// One-time gate cache per process. tagEnabled() is itself cheap, but the
// RAII scope caches the result once at solver entry so the hot inner loop
// only tests a local bool.

// Per-solve RAII timer + emitter. Construct at solver entry; set result
// fields before each return; dtor emits one "solve" event (only when active).
struct SolveScope {
	bool active;
	const char* variant;       // "jump" | "plain" | "escape"
	std::chrono::steady_clock::time_point t0;

	// result fields (filled by the solver before return)
	long  nodes;               // node expansions (popped from OPEN)
	long  openPeak;            // peak OPEN-list size
	int   len;                 // resulting path length (cells), -1 if none
	int   trunc;               // 1 if numCells > MAX_STEPS_PER_MOVEPATH
	int   fail;                // 1 if no goal found
	const MovePath* out = nullptr;  // the path this solve fills; hashed into the event
	const mc2::OpenHeap& heap;
	int initialAllocations;
	MoveLayerRevisions revisions;
	bool previousRevisionMatch = false;

	explicit SolveScope(mc2::czstring variantName, const mc2::OpenHeap& searchHeap)
		: active(mc2_diag::tagEnabled("PATH")),
		  variant(variantName),
		  nodes(0), openPeak(0), len(-1), trunc(0), fail(1),
		  heap(searchHeap), initialAllocations(searchHeap.allocations())
	{
		if (active) {
			// Count opportunities before attempting speculative work. Matching layer
			// revisions alone do not validate object generations or request inputs.
			static thread_local const MissionMap* previousMap = nullptr;
			static thread_local MoveLayerRevisions previousRevisions;
			if (GameMap) {
				revisions = GameMap->rev;
				previousRevisionMatch = previousMap == GameMap && previousRevisions == revisions;
				previousMap = GameMap;
				previousRevisions = revisions;
			}
			t0 = std::chrono::steady_clock::now();
		}
	}

	~SolveScope() {
		if (!active)
			return;
		auto t1 = std::chrono::steady_clock::now();
		double us = std::chrono::duration<double, std::micro>(t1 - t0).count();
		char buf[288];
		auto bufEnd = std::format_to_n(buf, sizeof(buf) - 1,
			"{{\"ev\":\"solve\",\"variant\":\"{}\",\"us\":{:.2f},\"nodes\":{},"
			"\"open_peak\":{},\"len\":{},\"trunc\":{},\"fail\":{},\"h\":\"{:016x}\",\"allocs\":{},\"same_rev\":{},\"valid_rev\":{}}}",
			variant, us, nodes, openPeak, len, trunc, fail, (out && !fail) ? hashMovePath(out) : 0ull,
			heap.allocations() - initialAllocations, previousRevisionMatch ? 1 : 0,
			(GameMap && GameMap->rev == revisions) ? 1 : 0);
		*bufEnd.out = '\0';
		mc2_diag::writeEvent("PATH", 1, 0, buf);
	}
};

// Map-load event (once per mission). Direct field reads, cheap.
inline void emitMap(int areas, int doors, int special, int cellW, int cellH) {
	if (!mc2_diag::tagEnabled("PATH"))
		return;
	char buf[256];
	long area = static_cast<long>(cellW) * static_cast<long>(cellH);
	auto bufEnd = std::format_to_n(buf, sizeof(buf) - 1,
		"{{\"ev\":\"map\",\"areas\":{},\"doors\":{},\"special\":{},"
		"\"cellW\":{},\"cellH\":{},\"cellArea\":{}}}",
		areas, doors, special, cellW, cellH, area);
	*bufEnd.out = '\0';
	mc2_diag::writeEvent("PATH", 1, 0, buf);
}

// Per-tick queue work and explicit player-order latency. Maxima and sample
// counts are not percentiles; zero player samples cannot prove player latency.
inline void emitFrame(long queued, long processed, long backlog, long peak,
                      long oldestAge, int capHit, std::uint64_t expansions,
                      long playerServed, long playerMaxAge,
                      long playerPending, long playerPendingMaxAge) {
	if (!mc2_diag::tagEnabled("PATH"))
		return;
	if (queued == 0 && processed == 0 && backlog == 0)
		return;
	char buf[512];
	auto bufEnd = std::format_to_n(buf, sizeof(buf) - 1,
		"{{\"ev\":\"frame\",\"queued\":{},\"processed\":{},\"backlog\":{},\"peak\":{},"
		"\"oldest_age\":{},\"cap_hit\":{},\"expansions\":{},\"player_served\":{},"
		"\"player_max_age\":{},\"player_pending\":{},\"player_pending_max_age\":{}}}",
		queued, processed, backlog, peak, oldestAge, capHit, expansions,
		playerServed, playerMaxAge, playerPending, playerPendingMaxAge);
	*bufEnd.out = '\0';
	mc2_diag::writeEvent("PATH", 1, 0, buf);
}

} // namespace mc2_path_trace
