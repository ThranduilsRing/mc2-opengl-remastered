//-------------------------------------------------------------------------------------------------
// EditorPlaytest.h -- "Playtest in Game" controller for the MC2 Mission Editor.
//
// One-click loop: save the current mission -> launch mc2.exe as a child process with
// `-mission <pak>` -> capture combined stdout/stderr + exit code (via EditorTaskRunner)
// -> archive the captured log next to the mission.  Editor-only; no game/engine edits.
//
// THREADING: launches via EditorTaskRunner (worker thread + pipe).  All state mutation
// and the log-archive callback run on the MAIN (editor) thread, driven by
// EditorTaskRunner::PumpMainThread().  Do NOT call any method here off the main thread.
//
// Playtest saves in place and launches that pak. EditorData::save() updates
// editor path state and writes .old backups, so this also changes the active save.
//-------------------------------------------------------------------------------------------------
#ifndef EDITOR_PLAYTEST_H
#define EDITOR_PLAYTEST_H

#include <string>
#include <vector>

namespace EditorPlaytest
{
	enum class State { Idle, Running };

	// One parsed [MOVER v1] telemetry record from the running playtest child.
	// Populated by the runtime bridge v0 (stdout); see EditorPlaytest.cpp parser.
	struct MoverSnapshot
	{
		long        id        = -1;
		std::string name;
		long        team      = 0;
		float       x = 0.f, y = 0.f, z = 0.f;
		float       hp        = 0.f;   // 0..1 (1 = undamaged)
		std::string pilot;
		long        orderVal  = -1;
		std::string orderName;
		long        target    = -1;
		// v2 brain/path/target detail ([MOVER v2]). Defaults match the engine's
		// "absent" sentinels so a v1-only stream (older game exe) reads cleanly.
		long        brain     = -1;            // ABL brain FSM state index (opaque)
		long        moveState = -1;            // moveOrders.moveState
		float       gx = 0.f, gy = 0.f, gz = 0.f;  // active-path world goal
		long        pathStep  = -1;            // current step index
		long        pathSteps = 0;             // total steps in active path
		long        pathCost  = -1;            // A* total path cost
		float       tx = 0.f, ty = 0.f, tz = 0.f;  // current target world pos
	};

	// Live movers from the most recent telemetry burst (empty until the running
	// child emits [MOVER v1] lines). Replaced wholesale each burst. Main-thread only.
	const std::vector<MoverSnapshot>& LiveMovers();

	// Wall-clock tick (GetTickCount) of the last completed telemetry burst, or 0
	// if none received this run. Lets the panel show staleness.
	unsigned long LiveMoversStamp();

	// True when a mission is loaded/saved-able AND no playtest is currently running.
	bool CanPlaytest();

	// True while a launched playtest child process is still alive.
	bool IsRunning();

	// True when no playtest child is running (the inverse of IsRunning()).
	// The headless `-playtest` CLI smoke polls this after Start() to detect
	// completion (the child exited and OnFinished/OnCancelled has run).
	bool IsIdle();

	// Exit code of the most recently completed playtest child, or -1 if none has
	// completed yet (or the launch was cancelled/never started). Valid once
	// IsIdle() is true after a Start(). Set on the main thread by OnFinished.
	int LastExitCode();

	// Archived log path of the most recently completed playtest child ("" if the
	// archive write failed or no run has completed). Valid once IsIdle() is true.
	const char* LastLogPath();

	// Detected mod id for the most recent run, or "" when the mission is not in a
	// mod. Valid once Start() has run.
	const char* LastModId();

	// SMOKE CHILD ENV (headless `-playtest` CLI only -- interactive playtest is
	// unchanged). When enabled, the next Start() appends gos_smoke's argv
	// (`--mission <stem> --duration <sec>`) AND sets MC2_SMOKE_MODE=1 in the
	// child env so the launched game auto-quits cleanly after <sec> seconds
	// instead of running forever. `seconds` clamps to [1, 600]; default 30.
	void SetSmokeChildEnv(bool enabled, int seconds);

	// Begin a playtest: save mission in place, resolve game exe, launch child.
	// On any pre-launch failure (no mission, exe missing) sets an error status and
	// returns without launching.  Safe to call only when CanPlaytest() is true.
	void Start();

	// Terminate the running playtest child (TerminateProcess via EditorTaskRunner).
	// No-op when idle.
	void Stop();

	// One-line status for the toolbar UI (state, exit code, log path, last line).
	const char* StatusLine();

	// Resolved/overridable game exe path (env MC2_PLAYTEST_EXE or probe fallback).
	// Returned buffer is editable in place by the UI (fixed-size).
	char* GameExePathBuffer();
	int   GameExePathBufferSize();
}

#endif // EDITOR_PLAYTEST_H
