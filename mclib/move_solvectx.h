//***************************************************************************
//
//	move_solvectx.h -- PATHFINDING-SOLVER-ISOLATION-1
//
//	Per-solve context for the A* solver. Reentrant-shaping ONLY (single-
//	threaded); NOT threading, NOT optimization. Under MC2_PATH_SOLVE_ISOLATED
//	(default-OFF) each solve owns its OPEN list + solve flags in a stack-local
//	SolveContext instead of the file-static globals (openList / JumpOnBlocked).
//	OFF the solver keeps using those globals verbatim, so OFF is byte-identical
//	to the pre-slice path.
//
//	Usage in a solver body:
//	    mc2_move::SolveContext ctx;                      // stack, at entry
//	    if (!ctx.isolated && !openList) { openList = new PriorityQueue; ... }
//	    PriorityQueue* const solveOpen = ctx.pq();       // global OFF, own ON
//	    ... solveOpen->clear() / insert / remove ...     // body uses solveOpen
//	    const bool jumpOnBlocked = ctx.jumpOnBlocked();  // snapshot ON, global OFF
//
//	HARD RULE: OFF must behave exactly as before. The gate is a cached bool;
//	when OFF, ctx.pq() == ::openList and ctx.jumpOnBlocked() == ::JumpOnBlocked,
//	so the solver resolves to the identical objects it did pre-slice.
//
//***************************************************************************
#pragma once

#include <cstdlib>
#include "pqueue.h"
#include "Gates/gate.h"

// The file-static solver globals live in move.cpp. Declared here so the
// SolveContext can alias them when the gate is OFF.
extern PriorityQueuePtr openList;   // shared OPEN list (OFF path)
extern bool JumpOnBlocked;          // caller-staged solve flag
extern bool FindingEscapePath;      // caller-staged bake flag
extern long RamObjectWID;           // caller-staged bake input

namespace mc2_move {

// One-shot process gate cache. Default-OFF: only "1" enables isolation.
inline bool pathSolveIsolated() {
	static const bool s_on = mc2::gate::get<"MC2_PATH_SOLVE_ISOLATED">();
	return s_on;
}

// Per-solve scratch container. Construct on the stack at solver entry.
//
//   OFF  : pq() returns the shared file-static ::openList; flags read globals.
//   ON   : pq() returns &ownPQ (a private per-solve PriorityQueue); flags are
//          SNAPSHOTTED from the globals at construction (the caller staged them
//          into the globals immediately before the solve; single-threaded, so
//          the snapshot is exact) and thereafter read only from the context.
//
// So during an ON solve NO file-static mutable solver state is read or written:
// the OPEN list is ownPQ and the flags are the snapshots.
struct SolveContext {
	bool          isolated;

	// flag snapshots (only consulted when isolated)
	bool          jumpOnBlockedSnap;
	bool          findingEscapePathSnap;
	long          ramObjectWIDSnap;

	// private per-solve OPEN list. Only touched when isolated.
	PriorityQueue ownPQ;

	SolveContext()
		: isolated(pathSolveIsolated()),
		  jumpOnBlockedSnap(false),
		  findingEscapePathSnap(false),
		  ramObjectWIDSnap(0)
	{
		if (isolated) {
			ownPQ.init(5000);
			jumpOnBlockedSnap     = JumpOnBlocked;
			findingEscapePathSnap = FindingEscapePath;
			ramObjectWIDSnap      = RamObjectWID;
		}
	}

	// The OPEN list this solve must use. OFF => the shared global (identical
	// to pre-slice); ON => the private per-solve queue.
	inline PriorityQueue* pq() {
		return isolated ? &ownPQ : openList;
	}

	// Solve flag: snapshot when isolated, live global otherwise.
	inline bool jumpOnBlocked() const {
		return isolated ? jumpOnBlockedSnap : JumpOnBlocked;
	}
};

} // namespace mc2_move
