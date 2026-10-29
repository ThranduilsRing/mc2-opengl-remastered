//***************************************************************************
//
//	sensor_trace.h -- SENSOR-SCAN-FACTS-1 telemetry helper.
//
//	Additive, gated, default-OFF. When MC2_DIAG_TAGS lacks "SENSOR" the
//	mc2_diag::tagEnabled("SENSOR") check returns false, so the whole trace
//	is skipped: no counter accumulation, no snprintf, no I/O. The only
//	OFF-path cost is one cached bool + predicted branches.
//
//	HARD RULE: measure only. Nothing here changes sensor behavior, raises
//	any constant, or alters the O(sensors x movers) scan.
//
//	Mirrors mclib/path_trace.h (PATHFINDING-FACTS-1). The gate is the
//	mc2_diag "SENSOR" tag (MC2_DIAG_TAGS=SENSOR). A separate env gate
//	MC2_SENSOR_SCAN_FACTS is also honored so the counters can be armed
//	without widening the diag whitelist.
//
//***************************************************************************
#pragma once

#include "../GameOS/gameos/diagnostic_trace.h"
#include <cstdio>
#include "Gates/gate.h"
#include <cstdlib>
#include <chrono>
#include "mc2fmt.h"

namespace mc2_sensor_trace {

// One-time gate cache. The TeamSensorSystem::update driver caches the gate
// once per team-update (activeThisFrame), and the hot scanBattlefield inner
// loop only reads that local bool + increments plain longs -- no per-pair
// tagEnabled() call. Byte-identical when OFF.
struct Counters {
	bool active;         // gate cached for the current team-update
	long pairsTested;    // TRUE O(n.m) inner-loop iteration count (headline)
	long numSensors;     // sensors scanned this team-update
	long numMovers;      // movers in the dense moverList this frame
	long adds;           // contacts added this team-update
	long removes;        // contacts removed this team-update
	long modifies;       // contacts modified this team-update
	long candidates;     // conservative spatial-grid candidates
	long neighbors;      // candidates inside the exact scan radius
	long losBatches;
	long losRays;
	long singleBlockBatches; // batches with no inter-block parallelism

	void reset(bool a) {
		active = a;
		pairsTested = numSensors = numMovers = 0;
		adds = removes = modifies = 0;
		candidates = neighbors = losBatches = losRays = singleBlockBatches = 0;
	}
};

// Translation-unit-local accumulator, defined in contact.cpp.
extern Counters g_counters;

// Resolve the gate once. True when the "SENSOR" diag tag is whitelisted OR
// MC2_SENSOR_SCAN_FACTS is set to a non-zero value. Cached process-wide.
inline bool gateActive() {
	static int cached = -1;  // -1 unresolved, 0 off, 1 on
	if (cached < 0) {
		bool on = mc2_diag::tagEnabled("SENSOR");
		if (!on) {
			on = mc2::gate::get<"MC2_SENSOR_SCAN_FACTS">();
		}
		cached = on ? 1 : 0;
	}
	return cached == 1;
}

// Per-team-update RAII timer. Construct around the sensor scan loop; dtor
// emits one "frame" event (only when active). team = teamId round-robined
// this frame.
struct ScanScope {
	bool active;
	int  team;
	std::chrono::steady_clock::time_point t0;

	explicit ScanScope(int teamId)
		: active(gateActive()), team(teamId)
	{
		if (active) {
			g_counters.reset(true);
			t0 = std::chrono::steady_clock::now();
		}
	}

	~ScanScope() {
		if (!active) {
			g_counters.active = false;
			return;
		}
		auto t1 = std::chrono::steady_clock::now();
		double us = std::chrono::duration<double, std::micro>(t1 - t0).count();
		char buf[512];
		(*std::format_to_n(buf, (sizeof(buf)) - 1,
			"{{\"ev\":\"frame\",\"team\":{:d},\"num_sensors\":{:d},\"num_movers\":{:d},\"pairs_tested\":{:d},\"adds\":{:d},\"removes\":{:d},\"modifies\":{:d},\"candidates\":{:d},\"neighbors\":{:d},\"los_batches\":{:d},\"los_rays\":{:d},\"single_block_batches\":{:d},\"scan_us\":{:.2f}}}",
			mc2::fmt::as_i32(team), mc2::fmt::as_i32(g_counters.numSensors), mc2::fmt::as_i32(g_counters.numMovers),
			mc2::fmt::as_i32(g_counters.pairsTested), mc2::fmt::as_i32(g_counters.adds), mc2::fmt::as_i32(g_counters.removes),
			mc2::fmt::as_i32(g_counters.modifies), mc2::fmt::as_i32(g_counters.candidates),
			mc2::fmt::as_i32(g_counters.neighbors), mc2::fmt::as_i32(g_counters.losBatches),
			mc2::fmt::as_i32(g_counters.losRays), mc2::fmt::as_i32(g_counters.singleBlockBatches), us).out = '\0');
		mc2_diag::writeEvent("SENSOR", 1, 0, buf);
		g_counters.active = false;
	}
};

} // namespace mc2_sensor_trace
