// Pathfinding: GlobalMap doors: door/area links, link costs, path-cost lookups and the sector build.
// Declared in the class headers; shared names in move_internal.h.
#include "move_internal.h"
#include <mdspan>
#include <unordered_map>
#include <vector>


constexpr int MAX_DOORS_PER_SECTOR = 500;		// This should be WAY more than we'll ever need...

long maxNumDoors = 0;

void GlobalMap::calcGlobalDoors (void) {

	short doorMap[SECTOR_DIM][SECTOR_DIM];

	std::mdspan<short, std::dextents<std::size_t, 2>> areaView(areaMap, height, width);

	beginDoorProcessing();

	for (long sectorR = 0; sectorR < sectorHeight; sectorR++) {
		for (long sectorC = 0; sectorC < sectorWidth; sectorC++) {
			for (long dir = 1; dir < 3; dir++) {
				for (long x = 0; x < SECTOR_DIM; x++)
					for (long y = 0; y < SECTOR_DIM; y++)
						doorMap[x][y] = -1;

				minRow = sectorR * SECTOR_DIM;
				maxRow = minRow + SECTOR_DIM;
				minCol = sectorC * SECTOR_DIM;
				maxCol = minCol + SECTOR_DIM;

				for (long r = minRow; r < maxRow; r++)
					for (long c = minCol; c < maxCol; c++) {
						long curArea = areaView[r, c];
						if (curArea > -1) {
							// The doorMap tracks, for each tile, what area is adjacent
							// to it thru a door...
							long adjR = r + adjTile[dir][0];
							long adjC = c + adjTile[dir][1];
							if ((adjR >= 0) && (adjR < height) && (adjC >= 0) && (adjC < width)) {
								long adjArea = areaView[adjR, adjC];
								if ((adjArea > -1) && (curArea != adjArea)) {
									AreaType curAreaType = areas[curArea].type;
									AreaType adjAreaType = areas[adjArea].type;
									bool validDoor = false;
									if ((curAreaType == _AreaType::AREA_TYPE_NORMAL) && (adjAreaType == _AreaType::AREA_TYPE_NORMAL))
										validDoor = true;
									if (validDoor)
										doorMap[r - minRow][c - minCol] = adjArea;
								}
							}
						}
					}
		
				// Now, process the doors and add them to the global door list. The
				// direction we're currently processing will dictate how to
				// make our sweep across the sector map. So, we have a chunk a code for each
				// direction (THIS MUST BE MODIFIED IF WE INCLUDE DIAGONAL DIRECTIONS WHEN
				// MOVING ON THE GLOBAL MAP)...
				if (dir == 1) {
					for (long c = maxCol - 1; c >= minCol; c--) {
						long r = minRow;
						while (r < maxRow) {
							long adjArea = doorMap[r - minRow][c - minCol];
							if (adjArea > -1) {
								long curArea = areaView[r, c];
								long length = 0;
								while ((r < maxRow) && (areaView[r, c] == curArea) && (doorMap[r - minRow][c - minCol] == adjArea)) {
									length++;
									r++;
								}
								addDoor(curArea, adjArea, r - length, c, length, dir);
								}
							else
								r++;
						}
					}
					}
				else {
					for (long r = maxRow - 1; r >= minRow; r--) {
						long c = minCol;
						while (c < maxCol) {
							long adjArea = doorMap[r - minRow][c - minCol];
							if (adjArea > -1) {
								long curArea = areaView[r, c];
								long length = 0;
								while ((c < maxCol) && (areaView[r, c] == curArea) && (doorMap[r - minRow][c - minCol] == adjArea)) {
									length++;
									c++;
								}
								addDoor(curArea, adjArea, r, c - length, length, dir);
								}
							else
								c++;
						}
					}
				}
			}
		}
	}

	endDoorProcessing();
}

void GlobalMap::calcAreaDoors (void) {

	// One pass over the doors fills every area's run (door index ascending, as the per-area scans did): the per-area
	// scans were O(areas x doors), ~0.5 s per GlobalMap on a 3060-cell map.
	numDoorInfos = 0;
	std::vector<long> areaDoorCount(static_cast<size_t>(numAreas), 0);
	for (long d = 0; d < numDoors; d++) {
		areaDoorCount[doors[d].area[0]]++;
		if (doors[d].area[1] != doors[d].area[0])
			areaDoorCount[doors[d].area[1]]++;
	}
	for (long area = 0; area < numAreas; area++) {
		if (areaDoorCount[area] > 255)
			Fatal(areaDoorCount[area], " Too many area doors ");
		areas[area].numDoors = areaDoorCount[area];
		numDoorInfos += areas[area].numDoors;
	}
	// One pool for all areas, each area viewing its own run: the layout the packet-load path builds.
	doorInfos = mc2::globalmap::ownZeroed(doorInfosStore, static_cast<size_t>(numDoorInfos));
	long nextDoorInfo = 0;
	for (long area = 0; area < numAreas; area++) {
		if (areas[area].numDoors) {
			areas_doors[area] = &doorInfos[nextDoorInfo];
			nextDoorInfo += areas[area].numDoors;
			}
		else
			areas_doors[area] = nullptr;
	}
	std::fill(areaDoorCount.begin(), areaDoorCount.end(), 0);
	for (long d = 0; d < numDoors; d++)
		for (long side = 0; side < 2; side++) {
			const long area = doors[d].area[side];
			if (side == 1 && area == doors[d].area[0])
				continue;
			DoorInfoPtr info = &areas_doors[area][areaDoorCount[area]++];
			info->doorIndex = d;
			info->doorSide = (doors[d].area[1] == area);
		}
}

// Build-time link-cost memo. A sector whose 60x60 cells are all plain open ground (nothing off-map, wet, gated, blocked, forested
// or path-locked) costs the same A* for the same start/goal offsets wherever it sits, so a blank or mostly-open big map solves each
// distinct geometry once instead of once per door pair (3060-cell blank map: ~30 s -> under 1 s).
namespace {
std::vector<signed char> s_openSector;             // per sector: -1 unknown, 0 mixed, 1 uniformly open
std::unordered_map<uint32_t, long> s_openLinkCost; // (hover, start r/c, goal r/c within the sector) -> link cost

bool sectorUniformlyOpen (long mapULr, long mapULc) {
	for (long r = 0; r < SECTOR_DIM; r++)
		for (long c = 0; c < SECTOR_DIM; c++) {
			if (!GameMap->inBounds(mapULr + r, mapULc + c))
				return(false);
			MapCellPtr cell = GameMap->getCell(mapULr + r, mapULc + c);
			if (cell->getOffMap() || cell->getShallowWater() || cell->getDeepWater() || cell->getGate() ||
				!cell->getPassable() || cell->getForest() || cell->getPathlock(false))
				return(false);
		}
	return(true);
}
}

long GlobalMap::calcLinkCost (long startDoor, long thruArea, long goalDoor) {

	if ((doors[startDoor].area[0] != thruArea) && (doors[startDoor].area[1] != thruArea))
		return(-1);

	long startSide = (doors[startDoor].area[1] == thruArea);
	long startRow = doors[startDoor].row;
	long startCol = doors[startDoor].col;
	if (doors[startDoor].direction[0] == 1) {
		// Door goes from west to east
		// For now, use the midpoint of the door for purposes of calcing the cost between
		// doors...
		startRow += (doors[startDoor].length / 2);
		startCol += startSide;
		}
	else {
		// Door goes from north to south
		// For now, use the midpoint of the door for purposes of calcing the cost between
		// doors...
		startRow += startSide;
		startCol += (doors[startDoor].length / 2);
	}

	if ((doors[goalDoor].area[0] != thruArea) && (doors[goalDoor].area[1] != thruArea))
		return(-2);

	long goalSide = (doors[goalDoor].area[1] == thruArea);
	long goalRow = doors[goalDoor].row;
	long goalCol = doors[goalDoor].col;
	if (doors[goalDoor].direction[0] == 1) {
		// Door goes from west to east. For now, use the midpoint of the door
		// for purposes of calcing the cost between doors...
		goalRow += (doors[goalDoor].length / 2);
		goalCol += goalSide;
		}
	else {
		goalRow += goalSide;
		goalCol += (doors[goalDoor].length / 2);
	}

	glm::vec3 goalWorldPos;
	land->cellToWorld(goalRow, goalCol, goalWorldPos);

	long cost = 9999;
	
	if (!PathFindMap[SECTOR_PATHMAP]) {
		PathFindMap[SECTOR_PATHMAP] = MOVE_acquirePathFindMap(SECTOR_PATHMAP);
		PathFindMap[SECTOR_PATHMAP]->init(30, 30);
	}
	if (!PathFindMap[SIMPLE_PATHMAP]) {
		PathFindMap[SIMPLE_PATHMAP] = MOVE_acquirePathFindMap(SIMPLE_PATHMAP);
		PathFindMap[SIMPLE_PATHMAP]->init(SimpleMovePathRange * 2 + 1, SimpleMovePathRange * 2 + 1);
	}

	// Let's make sure the bridge tiles are NOT blocked here (since, during the
	// game, they won't be, unless destroyed, in which case we won't care since
	// they're their own area)...
	uint32_t openKey = 0;
	bool haveOpenKey = false;
	MovePath newPath;
	if (blank) {
		newPath.numSteps = 10;
		newPath.cost = 10;
		}
	else if ((areas[thruArea].type == _AreaType::AREA_TYPE_WALL) || (areas[thruArea].type == _AreaType::AREA_TYPE_GATE) || (areas[thruArea].type == _AreaType::AREA_TYPE_FOREST)) {
		// These costs are for when the area is open...
		newPath.numSteps = 10;
		newPath.cost = 10;
		}
	else {
		ClearBridgeTiles = true;
		long mapULr = areas[thruArea].sectorR * SECTOR_DIM;
		long mapULc = areas[thruArea].sectorC * SECTOR_DIM;
		const size_t sectorIndex = static_cast<size_t>(areas[thruArea].sectorR) * sectorWidth + areas[thruArea].sectorC;
		if (s_openSector.size() != static_cast<size_t>(sectorWidth) * sectorHeight)
			s_openSector.assign(static_cast<size_t>(sectorWidth) * sectorHeight, -1);
		if (s_openSector[sectorIndex] < 0)
			s_openSector[sectorIndex] = sectorUniformlyOpen(mapULr, mapULc) ? 1 : 0;
		if (s_openSector[sectorIndex] == 1) {
			openKey = (hover ? 1u << 24 : 0u) | (static_cast<uint32_t>(startRow - mapULr) << 18) | (static_cast<uint32_t>(startCol - mapULc) << 12) |
				(static_cast<uint32_t>(goalRow - mapULr) << 6) | static_cast<uint32_t>(goalCol - mapULc);
			auto hit = s_openLinkCost.find(openKey);
			if (hit != s_openLinkCost.end()) {
				ClearBridgeTiles = false;
				[[maybe_unused]] const auto& [cachedKey, cachedCost] = *hit;
				return(cachedCost);
			}
			haveOpenKey = true;
		}
		long moveParams = MOVEPARAM_NONE;
		if (hover)
			moveParams |= (MOVEPARAM_WATER_SHALLOW + MOVEPARAM_WATER_DEEP);
		else if (!blank)
			moveParams |= MOVEPARAM_WATER_SHALLOW;
		PathFindMap[SECTOR_PATHMAP]->setUp(mapULr,
					   mapULc,
					   SECTOR_DIM,
					   SECTOR_DIM,
					   hover ? 1 : 0,
					   nullptr,
					   startRow,
					   startCol,
					   goalWorldPos,
					   goalRow - mapULr,
					   goalCol - mapULc,
					   10,
					   0,
					   8,
					   MOVEPARAM_NONE);
		int goalCell[2];
		PathFindMap[SECTOR_PATHMAP]->calcPath(&newPath, nullptr, goalCell);
		ClearBridgeTiles = false;
	}

	// If there is no path, find out why! In theory, this should never occur...
	if (newPath.numSteps == 0) {
		// If the cost is 1, then our start and goal cells are the same. Thus, there
		// is a path, it's just REALLY short :)
		cost = (newPath.cost == 1) ? 1 : 9999;
		if (haveOpenKey)
			s_openLinkCost[openKey] = cost;
		return(cost);
		}
	else
		cost = newPath.getCost();
	gosASSERT(cost != 0);
	if (haveOpenKey)
		s_openLinkCost[openKey] = cost;
	return(cost);
}

void GlobalMap::changeAreaLinkCost (long area, long cost) {

	GlobalMapAreaPtr thruArea = &areas[area];
    DoorInfoPtr thruDoorInfo = areas_doors[area];
	long numDoors = thruArea->numDoors;
	for (long i = 0; i < numDoors; i++) {
		GlobalMapDoorPtr curDoor = &doors[thruDoorInfo[i].doorIndex];
		DoorInfoLinksPtr& curDoor_links = doors_links[thruDoorInfo[i].doorIndex];
		long doorSide = thruDoorInfo[i].doorSide;
		for (long j = 0; j < curDoor->numLinks[doorSide]; j++)
			curDoor_links[doorSide][j].cost = cost;
	}
}

void GlobalMap::calcDoorLinks (void) {
	s_openSector.clear();
	s_openLinkCost.clear();

	numDoorLinks = 0;

	// One pool for every link run, sized from the area door counts first: each door side views its own run of it, and the
	// pool is never regrown while those views are held.
	long maxDoors = 0;
	long linkTotal = 0;
	for (long d = 0; d < numDoors; d++)
		for (long s = 0; s < 2; s++) {
			long areaNumDoors = areas[doors[d].area[s]].numDoors;
			linkTotal += (areaNumDoors - 1) + NUM_EXTRA_DOOR_LINKS;
			if (areaNumDoors > maxDoors)
				maxDoors = areaNumDoors;
		}
	linkTotal += 2 * (maxDoors + NUM_EXTRA_DOOR_LINKS) + 2 * NUM_EXTRA_DOOR_LINKS;
	doorLinks = mc2::globalmap::ownZeroed(doorLinksStore, static_cast<size_t>(linkTotal));
	long nextLink = 0;
	auto takeRun = [&](long count) {
		DoorLinkPtr run = &doorLinks[nextLink];
		nextLink += count;
		return run;
	};

	maxDoors = 0;
	for (long d = 0; d < numDoors; d++) {
		GlobalMapDoorPtr thisDoor = &doors[d];
		DoorInfoLinksPtr& thisDoor_links = doors_links[d];
		for (long s = 0; s < 2; s++) {
			thisDoor->numLinks[s] = 0;
			thisDoor_links[s] = nullptr;

			long area = thisDoor->area[s];
			long areaNumDoors = areas[area].numDoors;
				thisDoor->numLinks[s] = areaNumDoors - 1;
				// Allocate enough links for all links to this door plus a scratch
				// link used during path calc...
				thisDoor_links[s] = takeRun(thisDoor->numLinks[s] + NUM_EXTRA_DOOR_LINKS);
				numDoorLinks += (thisDoor->numLinks[s] + NUM_EXTRA_DOOR_LINKS);
				long linkIndex = 0;
				for (long areaDoor = 0; areaDoor < areaNumDoors; areaDoor++) {
					long doorIndex = areas_doors[area][areaDoor].doorIndex;
					GlobalMapDoorPtr curDoor = &doors[doorIndex];
					if (curDoor != thisDoor) {
						thisDoor_links[s][linkIndex].doorIndex = doorIndex;
						thisDoor_links[s][linkIndex].doorSide = (curDoor->area[1] == area);
						thisDoor_links[s][linkIndex].cost = calcLinkCost(d, area, doorIndex);
						thisDoor_links[s][linkIndex].openCost = thisDoor_links[s][linkIndex].cost;
						linkIndex++;
					}
				}
			if (areaNumDoors > maxDoors)
				maxDoors = areaNumDoors;
		}
	}

	doors[numDoors + DOOR_OFFSET_START].numLinks[0] = maxDoors;
	numDoorLinks += (doors[numDoors + DOOR_OFFSET_START].numLinks[0] + NUM_EXTRA_DOOR_LINKS);
	doors_links[numDoors + DOOR_OFFSET_START][0] = takeRun(doors[numDoors + DOOR_OFFSET_START].numLinks[0] + NUM_EXTRA_DOOR_LINKS);
	doors[numDoors + DOOR_OFFSET_START].numLinks[1] = 0;
	numDoorLinks += (doors[numDoors + DOOR_OFFSET_START].numLinks[1] + NUM_EXTRA_DOOR_LINKS);
	doors_links[numDoors + DOOR_OFFSET_START][1] = takeRun(doors[numDoors + DOOR_OFFSET_START].numLinks[1] + NUM_EXTRA_DOOR_LINKS);

	doors[numDoors + DOOR_OFFSET_GOAL].numLinks[0] = maxDoors;
	numDoorLinks += (doors[numDoors + DOOR_OFFSET_GOAL].numLinks[0] + NUM_EXTRA_DOOR_LINKS);
	doors_links[numDoors + DOOR_OFFSET_GOAL][0] = takeRun(doors[numDoors + DOOR_OFFSET_GOAL].numLinks[0] + NUM_EXTRA_DOOR_LINKS);
	doors[numDoors + DOOR_OFFSET_GOAL].numLinks[1] = 0;
	numDoorLinks += (doors[numDoors + DOOR_OFFSET_GOAL].numLinks[1] + NUM_EXTRA_DOOR_LINKS);
	doors_links[numDoors + DOOR_OFFSET_GOAL][1] = takeRun(doors[numDoors + DOOR_OFFSET_GOAL].numLinks[1] + NUM_EXTRA_DOOR_LINKS);
	gosASSERT(nextLink == linkTotal);

	long numberL = 0;
	for (long i = 0; i < (numDoors + NUM_DOOR_OFFSETS); i++) {
		long numLinks = doors[i].numLinks[0] + NUM_EXTRA_DOOR_LINKS;
		gosASSERT(numLinks >= 2);
		numberL += numLinks;

		numLinks = doors[i].numLinks[1] + NUM_EXTRA_DOOR_LINKS;
		gosASSERT(numLinks >= 2);
		numberL += numLinks;
	}
	Assert(numberL == numDoorLinks, 0, " HUH ");
}

long GlobalMap::getPathCost (int startArea, int goalArea, bool withSpecialAreas, int& confidence, bool calcIt) {

	// This could be cleaned up if we re-save this data, 0 being no path and 1
	// indicating startArea == goalArea.
	if (startArea < 0)
		return(0);
	if (goalArea < 0)
		return(0);
	if (!areas[startArea].open)
		return(0);
	if (!areas[goalArea].open)
		return(0);
	if (startArea == goalArea)
		return(1);

	GlobalPathStep path[MAX_GLOBAL_PATH];
	if (withSpecialAreas)
		useClosedAreas = true;
	else
		useClosedAreas = false;
	long cost = calcPath(startArea, goalArea, path);
	useClosedAreas = false;
	confidence = GLOBAL_CONFIDENCE_GOOD;
	return(cost);
}

void GlobalMap::clearPathExistsTable (void) {

	if (!pathExistsTable)
		return;
	long tableSize = numAreas * (numAreas / 4 + 1);
	memset(pathExistsTable, GLOBALPATH_EXISTS_UNKNOWN, tableSize);
}

void GlobalMap::setPathExists (long fromArea, long toArea, unsigned char set) {

	if (!pathExistsTable)
		return;
	if (fromArea < 0)
		return;
	if (toArea < 0)
		return;

	long rowWidth = numAreas / 4 + 1;
	unsigned char* pathByte = pathExistsTable;
	pathByte += (rowWidth * fromArea + (toArea / 4));
	unsigned char pathShift = (toArea % 4) * 2;
	unsigned char pathBit = 0x03 << pathShift;
	*pathByte &= (pathBit ^ 0xFF);
	if (set)
		*pathByte |= (set << pathShift);
}

unsigned char GlobalMap::getPathExists (long fromArea, long toArea) {

	if (!pathExistsTable)
		return(false);
	if (fromArea < 0)
		return(false);
	if (toArea < 0)
		return(false);

	long rowWidth = numAreas / 4 + 1;
	unsigned char* pathByte = pathExistsTable;
	pathByte += (rowWidth * fromArea + (toArea / 4));
	unsigned char pathShift = (toArea % 4) * 2;
	unsigned char pathBit = 0x03 << pathShift;
	return(*pathByte & pathBit);
}

long GlobalMap::exitDirection (long doorIndex, long fromArea) {

	if (doors[doorIndex].area[0] == fromArea)
		return(doors[doorIndex].direction[0]);
	else if (doors[doorIndex].area[1] == fromArea)
		return(doors[doorIndex].direction[1]);
	return(-1);
}

void GlobalMap::getDoorTiles (long area, long door, GlobalMapDoorPtr areaDoor) {

	*areaDoor = doors[areas_doors[area][door].doorIndex];
}

bool GlobalMap::getAdjacentAreaCell (long area, long adjacentArea, long& cellRow, long& cellCol) {

	for (long i = 0; i < areas[area].numDoors; i++) {
		long doorIndex = areas_doors[area][i].doorIndex;
		long doorSide = areas_doors[area][i].doorSide;
		if (doors[doorIndex].area[doorSide % 1] == adjacentArea) {
			if ((doors[doorIndex].area[0] != area) && (doors[doorIndex].area[1] != area))
				STOP(("bad adjacent area door", nullptr));
			long goalSide = (doors[doorIndex].area[1] == adjacentArea);
			cellRow = doors[doorIndex].row;
			cellCol = doors[doorIndex].col;
			if (doors[doorIndex].direction[0] == 1) {
				// Door goes from west to east. For now, use the midpoint of the door
				// for purposes of calcing the cost between doors...
				cellRow += (doors[doorIndex].length / 2);
				cellCol += goalSide;
				}
			else {
				cellRow += goalSide;
				cellCol += (doors[doorIndex].length / 2);
			}
			return(true);
		}
	}
	return(false);
}

glm::vec3 GlobalMap::getDoorWorldPos (long area, long door, long* prevGoalCell) {

	long cellR, cellC;
	cellR = prevGoalCell[0];// + adjTile[areas[area].doors[door].direction][0];
	cellC = prevGoalCell[1];// += adjTile[areas[area].doors[door].direction][1];
	glm::vec3 pos;
	pos = glm::vec3(0.0f);
	pos.x = static_cast<float>(cellC) * Terrain::worldUnitsPerCell + Terrain::worldUnitsPerCell / 2 - Terrain::worldUnitsMapSide / 2;
	pos.y = (Terrain::worldUnitsMapSide / 2) - (static_cast<float>(cellR) * Terrain::worldUnitsPerCell) - Terrain::worldUnitsPerCell / 2;
	pos.z = land->getTerrainElevation(pos); // How do we get the elevation for this point? Do we care?
	return(pos);
}

long GlobalMap::build (MissionMapCellInfo* mapData) {

	#ifdef _DEBUG
	#endif

	// Note that h and w are in tiles.
	bool wasBlank = blank;
	init(GameMap->height, GameMap->width);
	blank = wasBlank;

	{ ZoneScopedN("GlobalMap::build setBuildNotSet");
	for (long r = 0; r < height; r++)
		for (long c = 0; c < width; c++)
			GameMap->setBuildNotSet(r, c, true);
	}

	numAreas = 0;
	numSpecialAreas = 0;
	specialAreas = nullptr;
	if (mapData) {
		specialAreas = mc2::globalmap::ownZeroed(specialAreasStore, static_cast<size_t>(MAX_SPECIAL_AREAS));
		calcSpecialAreas(mapData);
	}

	{ ZoneScopedN("GlobalMap::build calcAreas"); calcAreas(); }

	{ ZoneScopedN("GlobalMap::build calcCellsCovered"); calcCellsCovered(); }

	{ ZoneScopedN("GlobalMap::build calcGlobalDoors"); calcGlobalDoors(); }

	{ ZoneScopedN("GlobalMap::build calcSpecialTypes"); calcSpecialTypes(); }

	{ ZoneScopedN("GlobalMap::build calcAreaDoors"); calcAreaDoors(); }

	{ ZoneScopedN("GlobalMap::build calcDoorLinks"); calcDoorLinks(); }

	#ifdef _DEBUG
	#endif


	specialAreas = nullptr;
	std::vector<GlobalSpecialAreaInfo>().swap(specialAreasStore);

	return(NO_ERR);	
}
