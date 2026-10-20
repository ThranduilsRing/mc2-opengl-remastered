// Shared surface of the pathfinding translation units (move*.cpp).
//
// move.cpp keeps the data tables, MOVE_* entry points, MissionMap and MovePath; GlobalMap and MoveMap are split by
// responsibility. The cell-step helpers and the MoveMap inline cost/adjacency members live here so every hot
// search loop still inlines them.
#ifndef MOVE_INTERNAL_H
#define MOVE_INTERNAL_H

#ifndef MCLIB_H
#include"mclib.h"
#endif
#include"terrain_runtime.h"   // TERRAIN-RUNTIME-CONSUMER-WATER-1

#ifndef MOVE_H
#include"move.h"
#endif

#define	MOVELOG

#ifdef MOVELOG
#ifndef GAMELOG_H
#include"gamelog.h"
#endif
#endif

#include"gos_profiler.h"
#include"../GameOS/gameos/gos_terrain_indirect.h"  // PR2c Stage 1c — MarkMineDirty
#include"move_recon.h"  // MC2_MOVE_RECON per-frame pathfinding cost instrumentation
#include"path_trace.h"  // PATHFINDING-FACTS-1 Stage 1 gated PATH telemetry
#include"move_solvectx.h"  // PATHFINDING-SOLVER-ISOLATION-1 per-solve SolveContext
#include"globalmap_limits.h"   // the range checks of the counts a global move map reads from a pak

static_assert(mc2::globalmap::kMaxAreas == MAX_GLOBALMAP_AREAS && mc2::globalmap::kMaxDoors == MAX_GLOBALMAP_DOORS &&
              mc2::globalmap::kMaxCellsPerSide == MAX_MAP_CELL_WIDTH, "globalmap_limits.h must name the builder's limits");


#ifndef TEAM_H
using Relation = enum {
	RELATION_FRIENDLY,
	RELATION_NEUTRAL,
	RELATION_ENEMY,
	NUM_RELATIONS
};
inline constexpr int MAX_TEAMS = 8;
#endif


extern void memclear(void *Dest,int Length);

inline constexpr int NUM_CELL_OFFSETS = 128;

// Globals defined in move.cpp (or elsewhere in the engine) that the split pieces share.
extern float metersPerWorldUnit;
extern char* ExceptionGameMsg;
extern char ChunkDebugMsg[5120];
extern bool ZeroHPrime;
extern bool CullPathAreas;
extern bool debugMoveMap;
extern bool ClearBridgeTiles;
extern long RamObjectWID;
extern bool JumpOnBlocked;
extern bool FindingEscapePath;
extern bool BlockWallTiles;
extern MissionMapPtr GameMap;
extern GlobalMapPtr GlobalMoveMap[3];
extern float worldUnitsPerMeter;
extern long DebugMovePathType;
extern long SimpleMovePathRange;
extern char reverseDir[NUM_DIRECTIONS];
extern char rowShift[NUM_DIRECTIONS];
extern char colShift[NUM_DIRECTIONS];
extern signed char cellShift[NUM_CELL_OFFSETS * 2];
extern float cellShiftDistance[NUM_CELL_OFFSETS];
extern char reverseShift[NUM_CELL_OFFSETS];
extern long tileMulMAPCELL_DIM[MAX_MAP_CELL_WIDTH];
extern float MapCellDiagonal;
extern float HalfMapCell;
extern float VerticesMapSideDivTwo;
extern float MetersMapSideDivTwo;
extern bool IsDiagonalStep[NUM_CELL_OFFSETS];
extern char mineLayout[4][MAPCELL_DIM * MAPCELL_DIM];
extern char TeamRelations[MAX_TEAMS][MAX_TEAMS];
extern bool GoalIsDoor;
extern long numNodesVisited;
extern long topOpenNodes;
extern long MaxHPrime;
extern bool PreserveMapTiles;
extern MoveMapPtr PathFindMap[2];
MoveMapPtr MOVE_acquirePathFindMap (int which);
extern bool EditorSave;

inline constexpr int StepAdjDir[9] = {-1, 0, 2, 2, 4, 4, 6, 6, 0};

inline constexpr int adjTile[4][2] = {
	{-1, 0},
	{0, 1},
	{1, 0},
	{0, -1}
};

inline float agsqrt( float _a, float _b )
{
	return sqrt(_a*_a + _b*_b);
}

inline constexpr const char* typeString[] = {
    "normal area",
    "wall area",
    "gate area",
    "land bridge area",
    "forest area"
};


inline void calcAdjNode (long& r, long& c, long direction) {

	r += rowShift[direction];
	c += colShift[direction];
}

inline bool inMapBounds (int r, int c, int mapHeight, int mapWidth) {

	return((r >= 0) && (r < mapHeight) && (c >= 0) && (c < mapWidth));
}

inline bool MoveMap::adjacentCellOpen (long mapCellIndex, long dir) {

	long adjCellIndex = map[mapCellIndex].adjCells[dir];
	if (adjCellIndex == -1)
		return(false);

	if (map[adjCellIndex].flags & MOVEFLAG_MOVER_HERE)
		return(false);

	return(map[adjCellIndex].cost < COST_BLOCKED);
}

inline bool MoveMap::adjacentCellOpenJUMP (long r, long c, long dir) {

	long indexStart = dir * 2;
	long adjRow = r + cellShift[indexStart];
	long adjCol = c + cellShift[indexStart + 1];

	if (!inMapBounds(adjRow, adjCol, height, width))
		return(false);

	if (map[adjRow * maxWidth + adjCol].flags & MOVEFLAG_MOVER_HERE)
		return(false);

	return(map[adjRow * maxWidth + adjCol].cost < COST_BLOCKED);
}

inline int MoveMap::calcHPrime (int r, int c) {
	ZoneScopedN("GameLogic.PathManager.CalcPath3");
	long sum = 0;
	if (r > goalR)
		sum += (r - goalR);
	else
		sum += (goalR - r);

	if (c > goalC)
		sum += (c - goalC);
	else
		sum += (goalC - c);
	return(sum);
}

inline constexpr int MAX_MAPWIDTH = 1000;

inline void MoveMap::propogateCost (long mapCellIndex, long cost, long g) {

	gosASSERT(cost > 0);
	gosASSERT(g >= 0);
	MoveMapNodePtr curMapNode = &map[mapCellIndex];
	if (curMapNode->g > (g + cost)) {
		curMapNode->cost = cost;
		curMapNode->g = g + cost;
		curMapNode->fPrime = curMapNode->g + curMapNode->hPrime;
		if (curMapNode->flags & MOVEFLAG_OPEN) {
			reKeyOpen(mapCellIndex, curMapNode->fPrime);
			}
		else {
			for (long dir = 0; dir < numOffsets; dir++) {
				// First, make sure this is a legit direction to go. We do NOT
				// want to clip corners, so we'll check that here...
				bool isDiagonalWalk = IsDiagonalStep[dir];
				if (isDiagonalWalk) {

					// MINE CHECK should go in these adj tests...
					bool adj1Open = false;
					long adjCellIndex = map[mapCellIndex].adjCells[StepAdjDir[dir]];
					if (adjCellIndex > -1)
						if ((map[adjCellIndex].flags & MOVEFLAG_MOVER_HERE) == 0)
							adj1Open = (map[adjCellIndex].cost < COST_BLOCKED);

					bool adj2Open = false;
					adjCellIndex = map[mapCellIndex].adjCells[StepAdjDir[dir + 1]];
					if (adjCellIndex > -1)
						if ((map[adjCellIndex].flags & MOVEFLAG_MOVER_HERE) == 0)
							adj2Open = (map[adjCellIndex].cost < COST_BLOCKED);
					
					if (!adj1Open && !adj2Open)
						continue;
					
				}

				long succCellIndex = curMapNode->adjCells[dir];
				if (succCellIndex > -1) {
					MoveMapNodePtr succMapNode = &map[succCellIndex];
					if (succMapNode->cost < COST_BLOCKED)
						if ((succMapNode->hPrime != HPRIME_NOT_CALCED) && (succMapNode->hPrime < MaxHPrime)) {
							char dirToParent = reverseShift[dir];
							long cost = succMapNode->cost;
							// Diagonal movement is more costly...
							gosASSERT(cost > 0);
							if (isDiagonalWalk)
								cost += (cost << 1);
							if (dirToParent == succMapNode->parent)
								propogateCost(succCellIndex, cost, curMapNode->g);
							else if ((curMapNode->g + cost) < succMapNode->g) {
								succMapNode->parent = dirToParent;
								propogateCost(succCellIndex, cost, curMapNode->g);
							}
						}
				}
			}
		}
	}
}

inline void MoveMap::propogateCostJUMP (long r, long c, long cost, long g) {

	gosASSERT(cost > 0);
	gosASSERT(g >= 0);
	MoveMapNodePtr curMapNode = &map[r * maxWidth + c];
	if (curMapNode->g > (g + cost)) {
		curMapNode->cost = cost;
		curMapNode->g = g + cost;
		curMapNode->fPrime = curMapNode->g + curMapNode->hPrime;
		if (curMapNode->flags & MOVEFLAG_OPEN) {
			reKeyOpen(r * MAX_MAPWIDTH + c, curMapNode->fPrime);
			}
		else {
			long cellOffsetIndex = 0;
			for (long dir = 0; dir < numOffsets; dir++) {
				// First, make sure this is a legit direction to go. We do NOT
				// want to clip corners, so we'll check that here...
				bool isDiagonalWalk = IsDiagonalStep[dir];
				if (isDiagonalWalk) {
					if (!adjacentCellOpenJUMP(r, c, StepAdjDir[dir]) && !adjacentCellOpenJUMP(r, c, StepAdjDir[dir + 1])) {
						cellOffsetIndex += 2;
						continue;
					}
				}

				long succRow = r + cellShift[cellOffsetIndex++];
				long succCol = c + cellShift[cellOffsetIndex++];
				if (inMapBounds(succRow, succCol, height, width)) {
					MoveMapNodePtr succMapNode = &map[succRow * maxWidth + succCol];
					if (succMapNode->cost < COST_BLOCKED)
						if ((succMapNode->hPrime != HPRIME_NOT_CALCED) && (succMapNode->hPrime < MaxHPrime)) {
							char dirToParent = reverseShift[dir];

							bool jumping = false;
							long cost = succMapNode->cost;
							
							// Diagonal movement is more costly...
							gosASSERT(cost > 0);
							if (dir > 7) {
								jumping = true;
								if (solveJumpOnBlocked)   // PATHFINDING-SOLVER-ISOLATION-1
									cost = jumpCost;
								else
									cost += jumpCost;
								}
							else if (isDiagonalWalk)
								cost += (cost / 2);

							if (dirToParent == succMapNode->parent)
								propogateCostJUMP(succRow, succCol, cost, curMapNode->g);
							else if ((curMapNode->g + cost) < succMapNode->g) {
								succMapNode->parent = dirToParent;
								propogateCostJUMP(succRow, succCol, cost, curMapNode->g);
							}
						}
				}
			}
		}
	}
}

#endif // MOVE_INTERNAL_H
