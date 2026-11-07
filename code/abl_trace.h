//***************************************************************************
//
//	abl_trace.h -- ABL-VM-FACTS-1 telemetry helper.
//
//	Facts-only, additive, gated, default-OFF. When MC2_DIAG_TAGS lacks
//	"ABL" (and MC2_ABL_VM_FACTS is unset) the gateActive() check returns
//	false, so the whole trace is skipped: no accumulation, no snprintf,
//	no I/O. OFF-path cost is one cached bool + predicted branches.
//
//	HARD RULE: measure only. Nothing here changes VM behavior, raises any
//	constant, or alters the per-statement dispatch. The existing
//	execStatementCount / CallStackLevel / NumStateTransitions counters are
//	READ, not re-instrumented.
//
//	Mirrors code/sensor_trace.h (SENSOR-SCAN-FACTS-1). Gate = mc2_diag
//	"ABL" tag (MC2_DIAG_TAGS=ABL) OR env MC2_ABL_VM_FACTS (arm counters
//	without widening the diag whitelist).
//
//	Seams: the brain->execute() call sites -- warrior.cpp (per-warrior
//	brain, throttled ~2.25s staggered) and mission.cpp (mission brain,
//	once/frame). Per-fire values captured at fire EXIT; a per-frame
//	summary is emitted once/frame at the mission-brain seam (the natural
//	frame boundary), aggregated across all fires that frame.
//
//***************************************************************************
#pragma once

#include "mc2_czstring.h"
#include "../GameOS/gameos/diagnostic_trace.h"
#include <cstdio>
#include "Gates/gate.h"
#include <cstdlib>
#include <chrono>
#include <algorithm>
#include <cstdint>
#include <format>

namespace mc2_abl_trace {

// Resolve the gate once, process-wide. True when the "ABL" diag tag is
// whitelisted OR MC2_ABL_VM_FACTS is set non-zero. Cached so the hot
// CallStackLevel++ peak-tracker reads a single int, no getenv/whitelist.
inline bool gateActive() {
	static int cached = -1;  // -1 unresolved, 0 off, 1 on
	if (cached < 0) {
		bool on = mc2_diag::tagEnabled("ABL");
		if (!on) {
			on = mc2::gate::get<"MC2_ABL_VM_FACTS">();
		}
		cached = on ? 1 : 0;
	}
	return cached == 1;
}

// Peak call-depth tracker. Set to 0 at each fire entry (FireScope), bumped
// at the CallStackLevel++ site in ablxstmt.cpp (guarded by the cached gate
// so OFF is byte-identical). Read at fire exit.
extern int g_peakCallDepth;

// Called from the VM's CallStackLevel++ site. Cheap: one cached-bool branch,
// then a max. OFF = single predicted branch, no write.
inline void notePeakDepth(int level) {
	if (gateActive() && level > g_peakCallDepth)
		g_peakCallDepth = level;
}

// Per-frame accumulator across all brain fires (warrior + mission), keyed on
// the game frame. Flushed + reset at the once/frame mission-brain seam.
struct FrameCounters {
	long fires;      // brain->execute() calls reached this frame
	long stmts;      // sum of execStatementCount returns
	long states;     // sum of NumStateTransitions
	int  maxDepth;   // peak CallStackLevel across fires
	double us;       // summed VM wall-time across fires

	void reset() { fires = stmts = states = 0; maxDepth = 0; us = 0.0; }
};

// TU-local accumulator, defined in warrior.cpp.
extern FrameCounters g_frame;

// RAII around a single brain->execute(). Times the fire, resets the peak
// tracker at entry, and on dtor (when active) reads the just-returned
// statement count + NumStateTransitions + peak depth, emits one per-fire
// "fire" event, and folds the values into the per-frame accumulator.
struct FireScope {
	bool   active;
	const char* who;   // "warrior" | "mission"
	int    wid;        // vehicle WatchID for warriors, -1 for mission
	long*  retStmts;   // out-param: statement count captured at dtor
	long   nStates;    // read from NumStateTransitions at dtor
	std::chrono::steady_clock::time_point t0;

	FireScope(mc2::czstring who_, int wid_)
		: active(gateActive()), who(who_), wid(wid_), retStmts(nullptr), nStates(0)
	{
		if (active) {
			g_peakCallDepth = 0;
			t0 = std::chrono::steady_clock::now();
		}
	}

	// Records the fire. stmts = execute() return; states = NumStateTransitions.
	void record(long stmts, long states) {
		if (!active)
			return;
		auto t1 = std::chrono::steady_clock::now();
		double us = std::chrono::duration<double, std::micro>(t1 - t0).count();
		int depth = g_peakCallDepth;

		g_frame.fires++;
		g_frame.stmts += stmts;
		g_frame.states += states;
		g_frame.us += us;
		g_frame.maxDepth = std::max(g_frame.maxDepth, depth);

		char buf[256];
		(*std::format_to_n(buf, (sizeof(buf)) - 1,
			"{{\"ev\":\"fire\",\"who\":\"{}\",\"wid\":{:d},\"stmts\":{:d},\"us\":{:.2f},\"depth\":{:d},\"states\":{:d}}}",
			who ? who : "(null)", static_cast<int32_t>(wid), static_cast<int32_t>(stmts), us, static_cast<int32_t>(depth), static_cast<int32_t>(states)).out = '\0');
		mc2_diag::writeEvent("ABL", 1, 0, buf);
	}
};

// Emit the per-frame summary (only when active and at least one fire) and
// reset the accumulator. Called once/frame at the mission-brain seam AFTER
// its own fire has been recorded. frame = g_mc2FrameCounter, tick = brain
// tick index.
inline void flushFrame(unsigned long frame, unsigned long tick) {
	if (!gateActive()) {
		g_frame.reset();
		return;
	}
	if (g_frame.fires > 0) {
		char buf[224];
		(*std::format_to_n(buf, (sizeof(buf)) - 1,
			"{{\"ev\":\"frame\",\"tick\":{:d},\"fires\":{:d},\"stmts\":{:d},\"us\":{:.2f},\"maxDepth\":{:d},\"states\":{:d}}}",
			static_cast<uint32_t>(tick), static_cast<int32_t>(g_frame.fires), static_cast<int32_t>(g_frame.stmts), g_frame.us,
			static_cast<int32_t>(g_frame.maxDepth), static_cast<int32_t>(g_frame.states)).out = '\0');
		mc2_diag::writeEvent("ABL", 1, frame, buf);
	}
	g_frame.reset();
}

} // namespace mc2_abl_trace
