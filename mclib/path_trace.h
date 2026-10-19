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

#include "../GameOS/gameos/diagnostic_trace.h"
#include <cstdio>
#include <chrono>

namespace mc2_path_trace {

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

	explicit SolveScope(const char* variantName)
		: active(mc2_diag::tagEnabled("PATH")),
		  variant(variantName),
		  nodes(0), openPeak(0), len(-1), trunc(0), fail(1)
	{
		if (active)
			t0 = std::chrono::steady_clock::now();
	}

	~SolveScope() {
		if (!active)
			return;
		auto t1 = std::chrono::steady_clock::now();
		double us = std::chrono::duration<double, std::micro>(t1 - t0).count();
		char buf[256];
		snprintf(buf, sizeof(buf),
			"{\"ev\":\"solve\",\"variant\":\"%s\",\"us\":%.2f,\"nodes\":%ld,"
			"\"open_peak\":%ld,\"len\":%d,\"trunc\":%d,\"fail\":%d}",
			variant, us, nodes, openPeak, len, trunc, fail);
		mc2_diag::writeEvent("PATH", 1, 0, buf);
	}
};

// Map-load event (once per mission). Direct field reads, cheap.
inline void emitMap(int areas, int doors, int special, int cellW, int cellH) {
	if (!mc2_diag::tagEnabled("PATH"))
		return;
	char buf[256];
	long area = static_cast<long>(cellW) * static_cast<long>(cellH);
	snprintf(buf, sizeof(buf),
		"{\"ev\":\"map\",\"areas\":%d,\"doors\":%d,\"special\":%d,"
		"\"cellW\":%d,\"cellH\":%d,\"cellArea\":%ld}",
		areas, doors, special, cellW, cellH, area);
	mc2_diag::writeEvent("PATH", 1, 0, buf);
}

// Per-frame queue event (only emit when there was path activity).
//
// PATHFINDING-FACTS-2 adds two high-concurrency probes:
//   oldestAge  -- frames the oldest request serviced this frame waited in queue
//                 (currentFrame - enqueueFrame); 0 if same-frame service, -1 if none serviced.
//   capHit     -- 1 if the numPathsToProcess=6 throttle left requests queued this
//                 frame (processed hit the cap with the queue still non-empty), else 0.
inline void emitFrame(long queued, long processed, long backlog, long peak,
                      long oldestAge, int capHit) {
	if (!mc2_diag::tagEnabled("PATH"))
		return;
	if (queued == 0 && processed == 0 && backlog == 0)
		return;  // skip idle frames to avoid spam
	char buf[224];
	snprintf(buf, sizeof(buf),
		"{\"ev\":\"frame\",\"queued\":%ld,\"processed\":%ld,\"backlog\":%ld,\"peak\":%ld,"
		"\"oldest_age\":%ld,\"cap_hit\":%d}",
		queued, processed, backlog, peak, oldestAge, capHit);
	mc2_diag::writeEvent("PATH", 1, 0, buf);
}

} // namespace mc2_path_trace
