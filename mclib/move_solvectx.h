//***************************************************************************
//
//	move_solvectx.h -- PATHFINDING-SOLVER-ISOLATION-1
//
//	Per-solve context for the A* solver. Reentrant-shaping ONLY (single-
//	threaded); NOT threading, NOT optimization. Under MC2_PATH_SOLVE_ISOLATED
//	(default-OFF) each solve reads its solve flags from a stack-local SolveContext
//	snapshot instead of the file-static globals (JumpOnBlocked / ...). OFF the
//	solver keeps using those globals verbatim, so OFF is byte-identical to the
//	pre-slice path. The OPEN list is no longer part of the context: every
//	GlobalMap and MoveMap owns its own (openHeap, mclib/open_heap.h).
//
//	Usage in a solver body:
//	    mc2_move::SolveContext ctx;                      // stack, at entry
//	    const bool jumpOnBlocked = ctx.jumpOnBlocked();  // snapshot ON, global OFF
//
//	HARD RULE: OFF must behave exactly as before. The gate is a cached bool;
//	when OFF, ctx.jumpOnBlocked() == ::JumpOnBlocked, so the solver resolves to
//	the identical objects it did pre-slice.
//
//***************************************************************************
#pragma once

#include <cstdlib>
#include "Gates/gate.h"

// The file-static solver globals live in move.cpp. Declared here so the
// SolveContext can alias them when the gate is OFF.
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
//   OFF  : the flags read the globals.
//   ON   : the flags are SNAPSHOTTED from the globals at construction (the
//          caller staged them into the globals immediately before the solve;
//          single-threaded, so the snapshot is exact) and thereafter read only
//          from the context.
struct SolveContext {
	bool          isolated;

	// flag snapshots (only consulted when isolated)
	bool          jumpOnBlockedSnap;
	bool          findingEscapePathSnap;
	long          ramObjectWIDSnap;

	SolveContext()
		: isolated(pathSolveIsolated()),
		  jumpOnBlockedSnap(false),
		  findingEscapePathSnap(false),
		  ramObjectWIDSnap(0)
	{
		if (isolated) {
			jumpOnBlockedSnap     = JumpOnBlocked;
			findingEscapePathSnap = FindingEscapePath;
			ramObjectWIDSnap      = RamObjectWID;
		}
	}

	// Solve flag: snapshot when isolated, live global otherwise.
	inline bool jumpOnBlocked() const {
		return isolated ? jumpOnBlockedSnap : JumpOnBlocked;
	}
};

} // namespace mc2_move
