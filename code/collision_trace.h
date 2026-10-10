//***************************************************************************
//
//	collision_trace.h -- COLLISION-FACTS-1 telemetry helper.
//
//	Additive, gated, default-OFF. When MC2_DIAG_TAGS lacks "COLLISION" and
//	MC2_COLLISION_FACTS is unset, gateActive() returns false, so the whole
//	trace is skipped: no counter accumulation, no snprintf, no I/O. The only
//	OFF-path cost is one cached bool + predicted branches.
//
//	HARD RULE: measure only. Nothing here changes collision behavior, raises
//	any constant, or alters the uniform-grid broadphase / narrow-phase.
//
//	Mirrors code/sensor_trace.h (SENSOR-SCAN-FACTS-1). The gate is the
//	mc2_diag "COLLISION" tag (MC2_DIAG_TAGS=COLLISION). A separate env gate
//	MC2_COLLISION_FACTS is also honored so the counters can be armed without
//	widening the diag whitelist.
//
//	Seam: CollisionSystem::checkObjects() (collsn.cpp) is the one per-frame
//	broadphase entry. The RAII CollideScope caches the gate at ctor, resets
//	the TU-local counters, times the whole broadphase+narrowphase, and emits
//	one JSONL "frame" event on dtor. The grid methods (createGrid/checkGrid)
//	touch g_counters directly, gated on g_counters.active (the cached bool).
//
//***************************************************************************
#pragma once

#include "../GameOS/gameos/diagnostic_trace.h"
#include <cstdio>
#include "Gates/gate.h"
#include <cstdlib>
#include <chrono>

namespace mc2_collision_trace {

// One-time gate cache. checkObjects caches the gate once per frame (active),
// and the hot grid loops only read that local bool + increment plain longs --
// no per-pair tagEnabled() call. Byte-identical when OFF.
struct Counters {
	bool active;         // gate cached for the current frame
	long pairsTested;    // checkGrid inner-loop iterations (headline pair count)
	long collidables;    // objects entering the broadphase this frame
	long gridCells;      // non-empty grid cells swept this frame
	long narrowChecks;   // pairs surviving class-pair cull -> detectCollision
	long culledPairs;    // pairs rejected by the class-pair cull
	long hits;           // actual collisions handled (checkExtents past guard)

	void reset(bool a) {
		active = a;
		pairsTested = collidables = gridCells = 0;
		narrowChecks = culledPairs = hits = 0;
	}
};

// Translation-unit-local accumulator, defined in collsn.cpp.
extern Counters g_counters;

// Resolve the gate once. True when the "COLLISION" diag tag is whitelisted OR
// MC2_COLLISION_FACTS is set to a non-zero value. Cached process-wide.
inline bool gateActive() {
	static int cached = -1;  // -1 unresolved, 0 off, 1 on
	if (cached < 0) {
		bool on = mc2_diag::tagEnabled("COLLISION");
		if (!on) {
			on = mc2::gate::get<"MC2_COLLISION_FACTS">();
		}
		cached = on ? 1 : 0;
	}
	return cached == 1;
}

// Per-frame RAII timer. Construct at the top of checkObjects; dtor emits one
// "frame" event (only when active).
struct CollideScope {
	bool active;
	std::chrono::steady_clock::time_point t0;

	CollideScope()
		: active(gateActive())
	{
		if (active) {
			g_counters.reset(true);
			t0 = std::chrono::steady_clock::now();
		}
	}

	~CollideScope() {
		if (!active) {
			g_counters.active = false;
			return;
		}
		auto t1 = std::chrono::steady_clock::now();
		double us = std::chrono::duration<double, std::micro>(t1 - t0).count();
		char buf[320];
		snprintf(buf, sizeof(buf),
			"{\"ev\":\"frame\",\"collidables\":%ld,\"pairs_tested\":%ld,"
			"\"narrow\":%ld,\"culled\":%ld,\"grid_cells\":%ld,\"hits\":%ld,"
			"\"collide_us\":%.2f}",
			g_counters.collidables, g_counters.pairsTested,
			g_counters.narrowChecks, g_counters.culledPairs,
			g_counters.gridCells, g_counters.hits, us);
		mc2_diag::writeEvent("COLLISION", 1, 0, buf);
		g_counters.active = false;
	}
};

} // namespace mc2_collision_trace
