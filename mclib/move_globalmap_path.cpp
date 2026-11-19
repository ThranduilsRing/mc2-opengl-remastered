// Pathfinding: GlobalMap path search: start/goal doors, area ownership, calcPath, open/close and the debug log.
// Declared in the class headers; shared names in move_internal.h.
#include <algorithm>

#include "move_internal.h"
#include "mc2fmt.h"

#include "path_work.h"
namespace {

// WHAT: gives the open-list entry of `door` its new key.
// WHY: a door flagged OPEN that the heap does not hold must not be re-keyed from a wrong slot (rekey() answers false and touches nothing), so the door is
// skipped. The first miss of a map logs one line (later misses are silent); release builds compile the assert out.
void reKeyOpenDoor(mc2::OpenHeap& open, long door, long newKey) {
	if (open.rekey(static_cast<uint32_t>(door), static_cast<int32_t>(newKey)))
		return;
	gosASSERT(false);
	if (open.rekeyMisses() == 1)
		mc2::fmt::print_err("[PATH v1] event=open_list_miss solver=globalmap id={:d}\n", mc2::fmt::as_i32(door));
}

}  // namespace


void GlobalMap::setStartDoor (long startArea) {

	GlobalMapDoorPtr startDoor = &doors[numDoors + DOOR_OFFSET_START];
	const DoorInfoLinksPtr& startDoor_links = doors_links[numDoors + DOOR_OFFSET_START];
	startDoor->row = 0;
	startDoor->col = 0;
	startDoor->length = 0;
	startDoor->open = true;
	startDoor->teamID = -1;
	startDoor->area[0] = startArea;
	startDoor->area[1] = startArea;
	startDoor->areaCost[0] = 1;
	startDoor->areaCost[1] = 1;
	startDoor->direction[0] = -1;
	startDoor->direction[1] = -1;
	startDoor->numLinks[0] = areas[startArea].numDoors;
	startDoor->numLinks[1] = 0;
	startDoor->fromAreaIndex = 1;

	for (long curLink = 0; curLink < startDoor->numLinks[0]; curLink++) {
		const long doorIndex = areas_doors[startArea][curLink].doorIndex;
		const long doorSide = areas_doors[startArea][curLink].doorSide;
		GlobalMapDoorPtr curDoor = &doors[doorIndex];
		const DoorInfoLinksPtr& curDoor_links = doors_links[doorIndex];
		long costSum = 1;
		if (startCell[0] > -1) {
			if (startCell[0] > curDoor->row)
				costSum += (startCell[0] - curDoor->row);
			else	
				costSum += (curDoor->row - startCell[0]);

			if (startCell[1] > curDoor->col)
				costSum += (startCell[1] - curDoor->col);
			else
				costSum += (curDoor->col - startCell[1]);
		}

        
		startDoor_links[0][curLink].doorIndex = doorIndex;
		startDoor_links[0][curLink].doorSide = doorSide;
		startDoor_links[0][curLink].cost = costSum;
		startDoor_links[0][curLink].openCost = costSum;
		

		curDoor_links[doorSide][curDoor->numLinks[doorSide]].doorIndex = numDoors + DOOR_OFFSET_START;
		curDoor_links[doorSide][curDoor->numLinks[doorSide]].doorSide = 0;
		curDoor_links[doorSide][curDoor->numLinks[doorSide]].cost = costSum;
		curDoor_links[doorSide][curDoor->numLinks[doorSide]].openCost = costSum;

		curDoor->numLinks[doorSide]++;
	}

}

void GlobalMap::resetStartDoor (long startArea) {

	GlobalMapDoorPtr startDoor = &doors[numDoors + DOOR_OFFSET_START];
	
	for (long curLink = 0; curLink < startDoor->numLinks[0]; curLink++) {
		long doorSide = areas_doors[startArea][curLink].doorSide;
		doors[areas_doors[startArea][curLink].doorIndex].numLinks[doorSide]--;
	}
}

void GlobalMap::setAreaTeamID (long area, char teamID) {

	// NOTE: This assumes there won't be adjacent areas with team alignments.
	// If there are, we should record two teamIDs for each door (to track
	// the teamID for each "side" of the door).
	if (area < 0)
		return;

	GlobalMapAreaPtr curArea = &areas[area];
	const DoorInfoPtr curArea_doors = areas_doors[area];
	curArea->teamID = teamID;
	for (long d = 0; d < curArea->numDoors; d++) {
		doors[curArea_doors[d].doorIndex].teamID = teamID;
	}
}

void GlobalMap::setAreaOwnerWID (long area, long objWID) {

	if (area < 0)
		return;

	areas[area].ownerWID = objWID;
}

void GlobalMap::setGoalDoor (long goalArea) {

	if ((goalArea < 0) || (goalArea >= numAreas)) {
		char errMsg[256];
		mc2::fmt::to_buf(errMsg, " GlobalMap.setGoalDoor: bad goalArea ({:d} of {:d}) ", mc2::fmt::as_i32(goalArea), mc2::fmt::as_i32(numAreas));
		gosASSERT((goalArea >= 0) && (goalArea < numAreas));
	}

	GlobalMapDoorPtr goalDoor = &doors[numDoors + DOOR_OFFSET_GOAL];
	DoorInfoLinksPtr& goalDoor_links = doors_links[numDoors + DOOR_OFFSET_GOAL];

	goalSector[0] = areas[goalArea].sectorR;
	goalSector[1] = areas[goalArea].sectorC;

	goalDoor->row = 0;
	goalDoor->col = 0;
	goalDoor->length = 0;
	goalDoor->open = true;
	goalDoor->teamID = -1;
	goalDoor->area[0] = goalArea;
	goalDoor->area[1] = goalArea;
	goalDoor->areaCost[0] = 1;
	goalDoor->areaCost[1] = 1;
	goalDoor->direction[0] = -1;
	goalDoor->direction[1] = -1;
	goalDoor->numLinks[0] = areas[goalArea].numDoors;
	goalDoor->numLinks[1] = 0;
	
	for (long curLink = 0; curLink < goalDoor->numLinks[0]; curLink++) {
		long doorIndex = areas_doors[goalArea][curLink].doorIndex;
		long doorSide = areas_doors[goalArea][curLink].doorSide;
		gosASSERT((doorIndex >= 0) && (doorIndex < (numDoors + NUM_DOOR_OFFSETS)));
		GlobalMapDoorPtr curDoor = &doors[doorIndex];
	    DoorInfoLinksPtr& curDoor_links = doors_links[doorIndex];
		long costSum = 1;
		if (goalCell[0] > -1) {
			if (goalCell[0] > curDoor->row)
				costSum += (goalCell[0] - curDoor->row);
			else	
				costSum += (curDoor->row - goalCell[0]);

			if (goalCell[1] > curDoor->col)
				costSum += (goalCell[1] - curDoor->col);
			else
				costSum += (curDoor->col - goalCell[1]);
		}
		goalDoor_links[0][curLink].doorIndex = doorIndex;
		goalDoor_links[0][curLink].doorSide = doorSide;
		goalDoor_links[0][curLink].cost = costSum;
		goalDoor_links[0][curLink].openCost = costSum;
		
		curDoor_links[doorSide][curDoor->numLinks[doorSide]].doorIndex = numDoors + DOOR_OFFSET_GOAL;
		curDoor_links[doorSide][curDoor->numLinks[doorSide]].doorSide = 0;
		curDoor_links[doorSide][curDoor->numLinks[doorSide]].cost = costSum;
		curDoor_links[doorSide][curDoor->numLinks[doorSide]].openCost = costSum;
		curDoor->numLinks[doorSide]++;
	}
}

void GlobalMap::resetGoalDoor (long goalArea) {

	GlobalMapDoorPtr goalDoor = &doors[numDoors + DOOR_OFFSET_GOAL];
	
	for (long curLink = 0; curLink < goalDoor->numLinks[0]; curLink++) {
		long doorSide = areas_doors[goalArea][curLink].doorSide;
		doors[areas_doors[goalArea][curLink].doorIndex].numLinks[doorSide]--;
	}
}

constexpr int AREA_ID_BASE = 1000000;

long GlobalMap::calcHPrime (long door) {

	gosASSERT((door > -1) && (door < (numDoors + NUM_DOOR_OFFSETS)));
	long sectorR = (areas[doors[door].area[0]].sectorR + areas[doors[door].area[1]].sectorR) / 2;
	long sectorC = (areas[doors[door].area[0]].sectorC + areas[doors[door].area[1]].sectorC) / 2;

	long sum = 0;
	if (sectorR > goalSector[0])
		sum += (sectorR - goalSector[0]);
	else
		sum += (goalSector[0] - sectorR);

	if (sectorC > goalSector[1])
		sum += (sectorC - goalSector[1]);
	else
		sum += (goalSector[1] - sectorC);

	return(sum);	
}

inline void GlobalMap::propogateCost (long door, long cost, long fromAreaIndex, long g) {

	gosASSERT((door >= 0) && (door < (numDoors + NUM_DOOR_OFFSETS)) && ((fromAreaIndex == 0) || (fromAreaIndex == 1)));

	GlobalMapDoorPtr curMapDoor = &doors[door];
	const DoorInfoLinksPtr& curMapDoor_links = doors_links[door];
	if (curMapDoor->g > (g + cost)) {
		curMapDoor->g = g + cost;
		curMapDoor->fPrime = curMapDoor->g + curMapDoor->hPrime;
		if (curMapDoor->flags & MOVEFLAG_OPEN) {
			reKeyOpenDoor(openHeap, door, curMapDoor->fPrime);
			}
		else {
			long toAreaIndex = 1 - fromAreaIndex;
			long numLinks = curMapDoor->numLinks[toAreaIndex];
			for (long curLink = 0; curLink < numLinks; curLink++) {
				long succDoor = curMapDoor_links[toAreaIndex][curLink].doorIndex;
				gosASSERT((succDoor >= 0) && (succDoor < numDoors + NUM_DOOR_OFFSETS));
				GlobalMapDoorPtr succMapDoor = &doors[succDoor];
				long succDoorCost = curMapDoor_links[toAreaIndex][curLink].cost;
				if (useClosedAreas) {
					if ((succMapDoor->teamID > -1) && (succMapDoor->teamID != moverTeamID))
						succDoorCost = 1000;
					}
				else {
					if ((succMapDoor->teamID > -1) && (succMapDoor->teamID != moverTeamID))
						succDoorCost = COST_BLOCKED;
					if (!succMapDoor->open)
						succDoorCost = COST_BLOCKED;
				}
				long succFromAreaIndex = (succMapDoor->area[1] == curMapDoor->area[toAreaIndex]);
				if ((succMapDoor->open || useClosedAreas) && (succDoorCost < COST_BLOCKED))
					if ((succMapDoor->hPrime != HPRIME_NOT_CALCED)/* && (succMapDoor->hPrime < MaxHPrime)*/) {
						if (door == succMapDoor->parent)
							propogateCost(succDoor, succDoorCost, toAreaIndex, curMapDoor->g);
						else if ((curMapDoor->g + succDoorCost) < succMapDoor->g) {
							succMapDoor->cost = succDoorCost;
							succMapDoor->parent = door;
							succMapDoor->fromAreaIndex = succFromAreaIndex;
							propogateCost(succDoor, succDoorCost, succFromAreaIndex, curMapDoor->g);
						}
					}
			}
		}
	}
}

long GlobalMap::calcPath (long startArea,
						  long goalArea,
						  GlobalPathStepPtr path,
						  long startRow,
						  long startCol,
						  long goalRow,
						  long goalCol) {

	ZoneScopedN("Warrior.Path.SolveGlobal");
	mc2_path_trace::SolveScope _pt("global", openHeap);
	MoveReconScope _recon_ag(&g_moveRecon_astar_global_ns, &g_moveRecon_frame_astar_global_ns);
	if (g_moveReconEnabled) g_moveRecon_astar_global_calls++;

	#ifdef _DEBUG
	#endif

	if ((startArea == -1) || (goalArea == -1))
		return(-1);

	startCell[0] = startRow;
	startCell[1] = startCol;
	goalCell[0] = goalRow;
	goalCell[1] = goalCol;

	if (logEnabled) {
		char s[50];
		mc2::fmt::to_buf(s, "     start = {:d}, goal = {:d}", mc2::fmt::as_i32(startArea), mc2::fmt::as_i32(goalArea));
		log->write(s);
		if (useClosedAreas)
			log->write("     USE CLOSED AREAS");
	}

	// Door costs should be set here, if we want to modify them real-time...

	// We will not limit the search, as the global map should never be so big
	// that we'd WANT to limit the search. If so, time to change the sectorDim,
	// etc...

	// The open list is this map's own: sized to the door graph, empty at the start of every solve.
	openHeap.reserve(numDoors + NUM_DOOR_OFFSETS);

	// NOTE: The last 6 doors are reserved for use by the pathfinder:
	//			numDoors + 0 = startArea
	//			numDoors + 1 = goalArea
	//			numDoors + 2 thru 4 = doors for "blocked" start area
	const long startDoor = numDoors + DOOR_OFFSET_START;
	const long goalDoor = numDoors + DOOR_OFFSET_GOAL;

	// MC2_MOVE_RECON: size the per-call FIXED reinit (door-clear O(numDoors) +
	// gate-scan O(numAreas)) vs the A* search. Default OFF.
	std::chrono::steady_clock::time_point _ag_pro0;
	if (g_moveReconEnabled) {
		_ag_pro0 = std::chrono::steady_clock::now();
		g_moveRecon_ag_maxNumDoors = std::max(g_moveRecon_ag_maxNumDoors, static_cast<uint64_t>(numDoors));
		g_moveRecon_ag_maxNumAreas = std::max(g_moveRecon_ag_maxNumAreas, static_cast<uint64_t>(numAreas));
	}

	long initHPrime = ZeroHPrime ? 0 : HPRIME_NOT_CALCED;
	for (long d = 0; d < numDoors + NUM_DOOR_OFFSETS; d++) {
		doors[d].cost = 1;
		doors[d].parent = -1;
		doors[d].fromAreaIndex = -1;
		doors[d].flags = 0;
		doors[d].g = 0;
		doors[d].hPrime = initHPrime;
		doors[d].fPrime = 0;
	}
	
	setStartDoor(startArea);
	setGoalDoor(goalArea);

	if (areas[startArea].offMap)
		openArea(startArea);
	if (areas[goalArea].offMap)
		openArea(goalArea);

	if (!isGateOpenCallback || !isGateDisabledCallback)
		STOP(("Globalmap.calcPath: NULL gate callback"));

	for (long i = 0; i < numAreas; i++)
		if (areas[i].type == _AreaType::AREA_TYPE_GATE) {
			if (g_moveReconEnabled) g_moveRecon_ag_gatesScanned++;
			if ((areas[i].teamID == moverTeamID) || (areas[i].teamID == -1)) {
				if (areas[i].ownerWID > 0) {
					if (isGateDisabledCallback(areas[i].ownerWID))
						closeArea(i);
					else
						openArea(i);
					}
				else
					openArea(i);
				}
			else if (isGateOpenCallback(areas[i].ownerWID))
				openArea(i);
			else
				closeArea(i);
		}


	// Put the START vertex on the empty OPEN list...
	// MC2_MOVE_RECON: close prologue, open search timer.
	std::chrono::steady_clock::time_point _ag_srch0;
	if (g_moveReconEnabled) {
		auto _now = std::chrono::steady_clock::now();
		uint64_t _pro = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(_now - _ag_pro0).count());
		g_moveRecon_ag_prologue_ns += _pro;
		g_moveRecon_ag_max_prologue_ns = std::max(g_moveRecon_ag_max_prologue_ns, _pro);
		_ag_srch0 = _now;
	}

	PQNode initialVertex;
	initialVertex.key = 0;
	initialVertex.id = startDoor;
	openHeap.clear();
	openHeap.insert(initialVertex);
	doors[startDoor].flags |= MOVEFLAG_OPEN;


	bool goalFound = false;

	while (!openHeap.isEmpty()) {
		PQNode bestPQNode;
		bestPQNode = mc2::path_work::popNode(openHeap).value();
		if (g_moveReconEnabled) g_moveRecon_ag_nodesPopped++;
		long bestDoor = bestPQNode.id;
		GlobalMapDoorPtr bestMapDoor = &doors[bestDoor];
		const DoorInfoLinksPtr& bestMapDoor_links = doors_links[bestDoor];
		bestMapDoor->flags &= (MOVEFLAG_OPEN ^ 0xFFFFFFFF);

		long bestDoorG = bestMapDoor->g;

		long fromAreaIndex = bestMapDoor->fromAreaIndex;

		bestMapDoor->flags |= MOVEFLAG_CLOSED;
		
		if (bestDoor == goalDoor) {
			goalFound = true;
			break;
		}

		long toAreaIndex = 1 - fromAreaIndex;
		long thruArea = bestMapDoor->area[toAreaIndex];
		long numLinks = bestMapDoor->numLinks[toAreaIndex];
		
		if (logEnabled) {
			char s[50];
			mc2::fmt::to_buf(s, "     thruArea = {:d}, bestDoor = {:d}, numLinks = {:d}", mc2::fmt::as_i32(thruArea), mc2::fmt::as_i32(bestDoor), mc2::fmt::as_i32(numLinks));
			log->write(s);
		}
		
		for (long curLink = 0; curLink < numLinks; curLink++) {
			// If we want to limit the doors thru which the path may
			// travel, we must check right here...
			
			long succDoor = bestMapDoor_links[toAreaIndex][curLink].doorIndex;
			gosASSERT((succDoor >= 0) && (succDoor < numDoors + NUM_DOOR_OFFSETS));
			GlobalMapDoorPtr succMapDoor = &doors[succDoor];

			if (logEnabled) {
				char s[50];
				mc2::fmt::to_buf(s, "          {:02d}) succDoor = {:d}", mc2::fmt::as_i32(curLink), mc2::fmt::as_i32(succDoor));
				log->write(s);
			}

			long succDoorCost = bestMapDoor_links[toAreaIndex][curLink].cost;
			// If this is an aligned door, make it more expensive for unfriendly movers...
			if (useClosedAreas) {
				if (succMapDoor->teamID > -1)
					if (TeamRelations[succMapDoor->teamID][moverTeamID] == RELATION_FRIENDLY)
						succDoorCost = 50;
				if (!succMapDoor->open)
					succDoorCost = 1000;
				}
			else {
				if ((succMapDoor->teamID > -1) && (succMapDoor->teamID != moverTeamID))
					succDoorCost = COST_BLOCKED;
				if (!succMapDoor->open)
					succDoorCost = COST_BLOCKED;
			}

			if (logEnabled) {
				char s[50];
				mc2::fmt::to_buf(s, "                  succDoorCost = {:d}", mc2::fmt::as_i32(succDoorCost));
				log->write(s);
			}

			if (succDoorCost < COST_BLOCKED) {
				if (succMapDoor->hPrime == HPRIME_NOT_CALCED)
					succMapDoor->hPrime = calcHPrime(succDoor);

				long succDoorG = bestDoorG + succDoorCost;

				long succFromAreaIndex = (succMapDoor->area[1] == thruArea);
				if (succMapDoor->flags & MOVEFLAG_OPEN) {
					if (succDoorG < succMapDoor->g) {
						succMapDoor->cost = succDoorCost;
						succMapDoor->parent = bestDoor;
						succMapDoor->fromAreaIndex = succFromAreaIndex;
						succMapDoor->g = succDoorG;
						succMapDoor->fPrime = succDoorG + succMapDoor->hPrime;
						reKeyOpenDoor(openHeap, succDoor, succMapDoor->fPrime);
					}
					}
				else if (succMapDoor->flags & MOVEFLAG_CLOSED) {
					if (succDoorG < succMapDoor->g) {
						succMapDoor->cost = succDoorCost;
						succMapDoor->parent = bestDoor;
						succMapDoor->fromAreaIndex = succFromAreaIndex;
						propogateCost(succDoor, succDoorCost, succFromAreaIndex, bestDoorG);
					}
					}
				else {
					succMapDoor->cost = succDoorCost;
					succMapDoor->parent = bestDoor;
					succMapDoor->fromAreaIndex = succFromAreaIndex;
					succMapDoor->g = succDoorG;
					succMapDoor->fPrime = succDoorG + succMapDoor->hPrime;
					PQNode succPQNode;
					succPQNode.key = succMapDoor->fPrime;
					succPQNode.id = succDoor;
					openHeap.insert(succPQNode);
					succMapDoor->flags |= MOVEFLAG_OPEN;
				}
			}
		}
	}

	// MC2_MOVE_RECON: close search timer.
	if (g_moveReconEnabled) {
		uint64_t _srch = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::steady_clock::now() - _ag_srch0).count());
		g_moveRecon_ag_search_ns += _srch;
		g_moveRecon_ag_max_search_ns = std::max(g_moveRecon_ag_max_search_ns, _srch);
	}

	resetStartDoor(startArea);
	resetGoalDoor(goalArea);

	if (areas[startArea].offMap)
		closeArea(startArea);
	if (areas[goalArea].offMap)
		closeArea(goalArea);

	if (goalFound) {
		long curDoor = goalDoor;
		long numSteps = 1;
		while (curDoor != startDoor) {
			numSteps++;
			curDoor = doors[curDoor].parent;
		}
		
		// Number of doors to travel to, including the "goal door". We don't care
		// about the "start door" in the path...
		long numDoors = numSteps - 1;
		if (numDoors > MAX_GLOBAL_PATH) {
			mc2::fmt::print_err("[PATH v1] event=global_path_capacity steps={:d} capacity={:d}\n",
				mc2::fmt::as_i32(numDoors), mc2::fmt::as_i32(MAX_GLOBAL_PATH));
			return 0;
		}

		curDoor = goalDoor;
		long curPathDoor = numDoors;

		long costToGoal = 0;
		while (curPathDoor > 0) {
			path[curPathDoor - 1].thruArea = doors[curDoor].area[doors[curDoor].fromAreaIndex];
			path[curPathDoor - 1].goalDoor = curDoor;
			path[curPathDoor - 1].costToGoal = costToGoal;
			costToGoal += doors[curDoor].cost;
			curDoor = doors[curDoor].parent;
			curPathDoor--;
		}

		#ifdef _DEBUG
		#endif

		if (logEnabled) {
			char s[50];
			mc2::fmt::to_buf(s, "     PATH FOUND: {:d} steps", mc2::fmt::as_i32(numDoors));
			log->write(s);
			log->write(" ");
		}
		return(numDoors);
		}
	else {
		if (logEnabled)
			log->write("     NO PATH FOUND");
	}
	if (logEnabled)
		log->write(" ");
	return(0);
}

long GlobalMap::calcPath (glm::vec3 start, glm::vec3 goal, GlobalPathStepPtr path) {

	int startR, startC;
	land->worldToCell(start, startR, startC);

	int goalR, goalC;
	land->worldToCell(goal, goalR, goalC);

	long numSteps = calcPath(calcArea(startR, startC), calcArea(goalR, goalC), path, startR, startC, goalR, goalC);
	return(numSteps);
}

void GlobalMap::openDoor (long door) {

	doors[door].cost = 10;
	doors[door].open = true;
}

void GlobalMap::closeDoor (long door) {

	doors[door].open = false;
}

void GlobalMap::closeArea (long area) {

	if (area < 0)
		return;

	GlobalMapAreaPtr closedArea = &areas[area];
    const DoorInfoPtr& closedArea_doors = areas_doors[area];
	closedArea->open = false;
	for (long d = 0; d < closedArea->numDoors; d++)
		closeDoor(closedArea_doors[d].doorIndex);

	for (long i = 0; i < closedArea->numDoors; i++) {
		GlobalMapDoorPtr curDoor = &doors[closedArea_doors[i].doorIndex];
		DoorInfoLinksPtr& curDoor_links = doors_links[closedArea_doors[i].doorIndex];
		long doorSide = closedArea_doors[i].doorSide;
		for (long j = 0; j < curDoor->numLinks[doorSide]; j++)
			curDoor_links[doorSide][j].cost = 1000;
	}

	closes = true;
}

void GlobalMap::openArea (long area) {

	if (area < 0)
		return;

	GlobalMapAreaPtr openedArea = &areas[area];
    const DoorInfoPtr& openedArea_doors = areas_doors[area];

	openedArea->open = true;
	for (long d = 0; d < openedArea->numDoors; d++) {
		long areaSide1 = doors[openedArea_doors[d].doorIndex].area[0];
		long areaSide2 = doors[openedArea_doors[d].doorIndex].area[1];
		if (!isClosedArea(areaSide1) && !isClosedArea(areaSide2))
			openDoor(openedArea_doors[d].doorIndex);
	}

	for (long i = 0; i < openedArea->numDoors; i++) {
		GlobalMapDoorPtr curDoor = &doors[openedArea_doors[i].doorIndex];
		DoorInfoLinksPtr& curDoor_links = doors_links[openedArea_doors[i].doorIndex];
		long doorSide = openedArea_doors[i].doorSide;
		for (long j = 0; j < curDoor->numLinks[doorSide]; j++)
			curDoor_links[doorSide][j].cost = curDoor_links[doorSide][j].openCost;
	}

	opens = true;
}

void GlobalMap::print (char* fileName) {

	if (areaMap)
		return;

	File debugFile;
	debugFile.create(fileName);

	std::mdspan<short, std::dextents<std::size_t, 2>> areaView(areaMap, static_cast<std::size_t>(height), static_cast<std::size_t>(width));
	char outString[500];
	for (long row = 0; row < height; row++) {
		outString[0] = '\0';
		for (long col = 0; col < width; col++) {
			if (areaView[row, col] == -2)
				strcat(outString, ">< ");
			else if (areaView[row, col] == -1)
				strcat(outString, "** ");
			else {
				char chStr[8];
				mc2::fmt::to_buf(chStr, "{:02x} ", mc2::fmt::as_u32(areaView[row, col]));
				strcat(outString, chStr);
			}
		}
		strcat(outString, "\n");
		debugFile.writeString(outString);
	}
	debugFile.writeString("\n");

	debugFile.close();
}

void GlobalMap::destroy (void) {

	#ifdef _DEBUG
	#endif

	// The stores own every buffer the pointers view (build and load alike); the views die with them.
	areaMap = nullptr;
	areas = nullptr;
	areas_cellsCovered = nullptr;
	areas_doors = nullptr;
	doors = nullptr;
	doors_links = nullptr;
	doorInfos = nullptr;
	doorLinks = nullptr;
	doorBuildList = nullptr;
	specialAreas = nullptr;
	releasePathExistsTable();
	std::vector<short>().swap(areaMapStore);
	std::vector<GlobalMapArea>().swap(areasStore);
	std::vector<short*>().swap(areasCellsCoveredStore);
	std::vector<DoorInfoPtr>().swap(areasDoorsStore);
	std::vector<GlobalMapDoor>().swap(doorsStore);
	std::vector<DoorLinkPtr>().swap(doorLinkViewStore);
	std::vector<DoorInfo>().swap(doorInfosStore);
	std::vector<DoorLink>().swap(doorLinksStore);
	std::vector<GlobalMapDoor>().swap(doorBuildStore);
	std::vector<GlobalSpecialAreaInfo>().swap(specialAreasStore);
}

void GlobalMap::releasePathExistsTable (void) {

	pathExistsTable = nullptr;
	std::vector<unsigned char>().swap(pathExistsStore);
}

bool GlobalMap::toggleLog (void) {

	if (!log) {
		GameLog::setup();
		log = GameLog::getNewFile();
		if (!log)
			Fatal(0, " Couldn't create lrmove log ");
		long err = log->open("lrmove.log");
		if (err)
			Fatal(0, " Couldn't open lrmove log ");
		GlobalMapPtr map = GlobalMoveMap[0];
		if (true) {
			char s[256];
			for (long i = 0; i < map->numDoors; i++) {
				mc2::fmt::to_buf(s, "door {:05d}, {}({:d}), areas {:d} & {:d}", mc2::fmt::as_i32(i), mc2::fmt::safe(map->doors[i].open ? "opened" : "CLOSED"), mc2::fmt::as_i32(map->doors[i].teamID), mc2::fmt::as_i32(map->doors[i].area[0]), mc2::fmt::as_i32(map->doors[i].area[1]));
				log->write(s);
				for (long side = 0; side < 2; side++) {
					mc2::fmt::to_buf(s, "     side {:d}", mc2::fmt::as_i32(side));
					log->write(s); 
					for (long j = 0; j < map->doors[i].numLinks[side]; j++) {
						mc2::fmt::to_buf(s, "          link {:03d}, to door {:05d}, cost {:d}",
							mc2::fmt::as_i32(j),
							mc2::fmt::as_i32(map->doors_links[i][side][j].doorIndex),
							mc2::fmt::as_i32(map->doors_links[i][side][j].cost));
						log->write(s); 
					}
				}
			}
			log->write(" "); 

			for (int i = 0; i < map->numAreas; i++) {
				mc2::fmt::to_buf(s, "area {:05d}, {}({:d})", mc2::fmt::as_i32(i), mc2::fmt::safe(map->areas[i].open ? "opened" : "CLOSED"), mc2::fmt::as_i32(map->areas[i].teamID));
				log->write(s); 
				if (map->areas[i].ownerWID > 0) {
					mc2::fmt::to_buf(s, "     ownerWID is {:d}", mc2::fmt::as_i32(map->areas[i].ownerWID));
					log->write(s);
				}
				mc2::fmt::to_buf(s, "     {}", mc2::fmt::safe(typeString[std::to_underlying(map->areas[i].type)]));
				log->write(s);
				for (long d = 0; d < map->areas[i].numDoors; d++) {
                    const DoorInfo& di = map->areas_doors[i][d];
					mc2::fmt::to_buf(s, "     door {:03d} is {:d} ({:d}:{:d} & {:d}) ",
						mc2::fmt::as_i32(d), mc2::fmt::as_i32(di.doorIndex), mc2::fmt::as_i32(di.doorSide),
						mc2::fmt::as_i32(map->doors[di.doorIndex].area[0]), mc2::fmt::as_i32(map->doors[di.doorIndex].area[1]));
					log->write(s);
				}
				
			}
			log->write(" ");
		}
	}
	logEnabled = !logEnabled;
	return(logEnabled);
}

void GlobalMap::writeLog (char* s) {

	if (log)
		log->write(s);
}
