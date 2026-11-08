//***************************************************************************
//
//	move_solvectx.h -- explicit path query and solver scratch for the MoveMap A* solvers
//
//	PathQuery is a value: the caller builds it and hands it to calcPath/calcPathJUMP/calcEscapePath.
//	The solver reads only the query it was given (it used to read caller-staged file-static globals).
//
//	SolverScratch owns the per-solve working state that is not the node grid: the OPEN list and
//	the per-solve cost cutoff and jump flag. A solve takes a SolverScratch by reference, so two
//	scratches on one prepared grid give identical results; nothing here is a global or a static.
//
//***************************************************************************
#pragma once

#include "open_heap.h"

namespace mc2_move {

struct PathQuery {
	// Jump moves replace the step cost instead of adding to it (elementals closing on a target).
	bool jumpOnBlocked = false;
};

struct SolverScratch {
	mc2::OpenHeap openHeap;       // OPEN list; each solve clears it before use
	long maxHPrime = 1000;        // hPrime ceiling, set at the top of each solve
	bool jumpOnBlocked = false;   // the query's flag, copied at the top of each solve
};

} // namespace mc2_move
