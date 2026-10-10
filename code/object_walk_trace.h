//***************************************************************************
//
//	object_walk_trace.h -- OBJECT-WALK-FACTS-1 telemetry helper.
//
//	Additive, gated, default-OFF. When MC2_DIAG_TAGS lacks "OBJWALK" and
//	MC2_OBJECT_WALK_FACTS is unset, gateActive() returns false, so the whole
//	trace is skipped: no counter accumulation, no snprintf, no I/O. The only
//	OFF-path cost is one cached bool + predicted branches.
//
//	HARD RULE: measure only. Nothing here changes object-walk behavior, raises
//	any constant, or alters the O(maxObjects) framesSinceActive sweep.
//
//	Mirrors code/sensor_trace.h (SENSOR-SCAN-FACTS-1). The gate is the mc2_diag
//	"OBJWALK" tag (MC2_DIAG_TAGS=OBJWALK). A separate env gate
//	MC2_OBJECT_WALK_FACTS is also honored so the counters can be armed without
//	widening the diag whitelist.
//
//***************************************************************************
#pragma once

#include "../GameOS/gameos/diagnostic_trace.h"
#include <cstdio>
#include "Gates/gate.h"
#include <cstdlib>
#include <chrono>

namespace mc2_object_walk_trace {

// One-time gate cache. GameObjectManager::update caches the gate once at the
// sweep-scope ctor; the hot for-loop only reads that local bool + increments
// plain longs -- no per-iteration tagEnabled() call. Byte-identical when OFF.
struct Counters {
	bool active;          // gate cached for the current sweep
	long objectsWalked;   // array slots iterated (== maxObjs) (headline)
	long liveObjects;     // non-null objList[i]
	long moversWalked;    // live obj->isMover() == true
	long staticsWalked;   // live obj->isMover() == false
	long activeThisFrame; // reset framesSinceActive = 0
	long agedThisFrame;   // incremented framesSinceActive (the aged majority)

	void reset(bool a) {
		active = a;
		objectsWalked = liveObjects = 0;
		moversWalked = staticsWalked = 0;
		activeThisFrame = agedThisFrame = 0;
	}
};

// Translation-unit-local accumulator, defined in objmgr.cpp.
extern Counters g_counters;

// Resolve the gate once. True when the "OBJWALK" diag tag is whitelisted OR
// MC2_OBJECT_WALK_FACTS is set to a non-zero value. Cached process-wide.
inline bool gateActive() {
	static int cached = -1;  // -1 unresolved, 0 off, 1 on
	if (cached < 0) {
		bool on = mc2_diag::tagEnabled("OBJWALK");
		if (!on) {
			on = mc2::gate::get<"MC2_OBJECT_WALK_FACTS">();
		}
		cached = on ? 1 : 0;
	}
	return cached == 1;
}

// Per-frame RAII timer. Construct around the framesSinceActive sweep loop; dtor
// emits one "frame" event (only when active).
struct SweepScope {
	bool active;
	std::chrono::steady_clock::time_point t0;

	SweepScope()
		: active(gateActive())
	{
		if (active) {
			g_counters.reset(true);
			t0 = std::chrono::steady_clock::now();
		}
	}

	~SweepScope() {
		if (!active) {
			g_counters.active = false;
			return;
		}
		auto t1 = std::chrono::steady_clock::now();
		double us = std::chrono::duration<double, std::micro>(t1 - t0).count();
		char buf[288];
		snprintf(buf, sizeof(buf),
			"{\"ev\":\"frame\",\"walked\":%ld,\"live\":%ld,\"movers\":%ld,"
			"\"statics\":%ld,\"active\":%ld,\"aged\":%ld,\"sweep_us\":%.2f}",
			g_counters.objectsWalked, g_counters.liveObjects,
			g_counters.moversWalked, g_counters.staticsWalked,
			g_counters.activeThisFrame, g_counters.agedThisFrame, us);
		mc2_diag::writeEvent("OBJWALK", 1, 0, buf);
		g_counters.active = false;
	}
};

} // namespace mc2_object_walk_trace
