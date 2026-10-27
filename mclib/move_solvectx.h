//***************************************************************************
//
//	move_solvectx.h -- per-solve context for the A* solver
//
//	The caller stages the solve flags into file-static globals (JumpOnBlocked, ...) immediately
//	before a solve. A solve snapshots them once at entry (SolveContext, on the stack) and reads
//	only the snapshot afterwards, so nothing the solver reads mid-solve is shared mutable state.
//	Every other per-solve value lives on the MoveMap (solveJumpOnBlocked, maxHPrime, openHeap,
//	the node grid). Output is identical to reading the globals: the solve never writes them.
//
//***************************************************************************
#pragma once

extern bool JumpOnBlocked;          // caller-staged solve flag
extern bool FindingEscapePath;      // caller-staged bake flag
extern long RamObjectWID;           // caller-staged bake input

namespace mc2_move {

struct SolveContext {
	bool jumpOnBlockedSnap = JumpOnBlocked;
	bool findingEscapePathSnap = FindingEscapePath;
	long ramObjectWIDSnap = RamObjectWID;

	bool jumpOnBlocked() const { return jumpOnBlockedSnap; }
};

} // namespace mc2_move
